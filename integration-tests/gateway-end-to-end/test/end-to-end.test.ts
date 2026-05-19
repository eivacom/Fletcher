// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end test that drives the real `gateway` exe (C++ WebSocket
// server) against the real gateway-client-ts (TypeScript
// FletcherClient).
//
// The gateway is built from gateway/src/ via add_subdirectory() in
// this directory's CMakeLists.txt. It is configured via
// test-config.yml, which pre-creates a single "telemetry" topic
// with the three-field schema {sensor_id : int32, temperature :
// float64, label : utf8} — enough to exercise the null bitfield,
// fixed-width, and variable-length encodings in one shot.
//
// The gateway exe is production-grade: no test-specific behaviour
// is baked in. The client-publish ⇄ self-subscription round-trip in
// InProcessProvider is what wires the "publishes from one client are
// delivered to subscribers (including the same client) over the
// WebSocket bus" assertion below.

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ChildProcess, spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  FletcherClient,
  WireTypeId,
  buildSubscribe,
  buildPublish,
  parseTextResponse,
  parseBinaryMessage,
  serializeEnvelope,
  encodePositional,
} from 'eiva-fletcher-gateway-client';
import type { SchemaDescriptor, SubscribedResponse } from 'eiva-fletcher-gateway-client';
import { TelemetrySchema } from '../generated-ts/telemetry.fletcher.js';

const here = fileURLToPath(new URL('.', import.meta.url));

const TEST_PORT  = 19091;
const TEST_URL   = `ws://127.0.0.1:${TEST_PORT}`;
const TEST_TOPIC = 'telemetry';
const CONFIG_PATH = resolve(here, '..', 'test-config.yml');

function findGatewayBinary(): string {
  if (process.env.GATEWAY_BIN) {
    return process.env.GATEWAY_BIN;
  }
  const candidates = [
    resolve(here, '..', 'build', 'Release', 'gateway_build', 'gateway'),
    resolve(here, '..', 'build', 'Release', 'gateway_build', 'gateway.exe'),
    resolve(here, '..', 'build', 'build', 'Release', 'gateway_build', 'gateway'),
  ];
  for (const c of candidates) {
    if (existsSync(c)) return c;
  }
  throw new Error(
    `gateway binary not found. Checked: ${candidates.join(', ')}. ` +
    `Set GATEWAY_BIN to override.`,
  );
}

// Spawn the gateway exe and resolve once it prints "READY <port>".
async function spawnGateway(): Promise<ChildProcess> {
  const bin = findGatewayBinary();
  const child = spawn(bin, [
    '--port', String(TEST_PORT),
    '--bind-address', '127.0.0.1',
    '--config', CONFIG_PATH,
  ], {
    stdio: ['pipe', 'pipe', 'pipe'],
  });

  child.stderr?.on('data', (chunk) => {
    process.stderr.write(`[gateway stderr] ${chunk.toString()}`);
  });

  return new Promise<ChildProcess>((resolveFn, rejectFn) => {
    const rl = createInterface({ input: child.stdout! });
    const timeout = setTimeout(() => {
      rejectFn(new Error('gateway did not print READY within 10 s'));
    }, 10_000);

    rl.on('line', (line) => {
      if (line.startsWith('READY ')) {
        clearTimeout(timeout);
        resolveFn(child);
      }
    });
    child.on('error', (err) => {
      clearTimeout(timeout);
      rejectFn(err);
    });
    child.on('exit', (code) => {
      clearTimeout(timeout);
      rejectFn(new Error(`gateway exited before READY (code=${code})`));
    });
  });
}

async function stopGateway(child: ChildProcess): Promise<void> {
  return new Promise<void>((resolveFn) => {
    child.on('exit', () => resolveFn());
    child.stdin?.write('stop\n');
    child.stdin?.end();
    setTimeout(() => {
      if (!child.killed) child.kill('SIGTERM');
      resolveFn();
    }, 5_000);
  });
}

let server: ChildProcess;

beforeAll(async () => {
  server = await spawnGateway();
});

afterAll(async () => {
  if (server) await stopGateway(server);
});

// ---------------------------------------------------------------------
// AC: schema delivery — both SchemaDescriptor (schema) and Arrow IPC
// (schemaIpc, base64) must be present in the subscribed response.
// We bypass FletcherClient for this case and inspect the raw JSON.
// ---------------------------------------------------------------------
describe('subscribed response — schema delivery', () => {
  it('carries both schema (SchemaDescriptor JSON) and schemaIpc (base64 Arrow IPC)',
     async () => {
    const ws = new WebSocket(TEST_URL);
    ws.binaryType = 'arraybuffer';

    const response = await new Promise<SubscribedResponse>((res, rej) => {
      ws.onopen = () => ws.send(buildSubscribe(TEST_TOPIC));
      ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
          const parsed = parseTextResponse(ev.data);
          if (parsed.type === 'subscribed') res(parsed);
          else if (parsed.type === 'error') rej(new Error(parsed.message));
        }
      };
      ws.onerror = () => rej(new Error('ws error'));
    });

    expect(response.topic).toBe(TEST_TOPIC);
    expect(response.subId).toBeTypeOf('bigint');

    // SchemaDescriptor JSON path — matches test-config.yml.
    expect(response.schema).toBeDefined();
    expect(response.schema!.fields).toHaveLength(3);
    expect(response.schema!.fields[0].name).toBe('sensor_id');
    expect(response.schema!.fields[0].wireType).toBe(WireTypeId.INT32);
    expect(response.schema!.fields[1].name).toBe('temperature');
    expect(response.schema!.fields[1].wireType).toBe(WireTypeId.FLOAT64);
    expect(response.schema!.fields[2].name).toBe('label');
    expect(response.schema!.fields[2].wireType).toBe(WireTypeId.STRING);

    // Arrow IPC bytes path.
    expect(response.schemaIpc).toBeTypeOf('string');
    expect(response.schemaIpc!.length).toBeGreaterThan(0);
    const ipcBytes = Buffer.from(response.schemaIpc!, 'base64');
    expect(ipcBytes.byteLength).toBeGreaterThan(0);

    ws.close();
  });
});

// ---------------------------------------------------------------------
// AC: client publish → server-side delivery → client receives. With
// InProcessProvider's loopback routing, a single client subscribing
// then publishing on the same topic is enough to prove the round-trip
// across the WebSocket. Covers both directions of the protocol.
// ---------------------------------------------------------------------
describe('client publish ↔ subscription round-trip', () => {
  it('delivers multiple distinct published rows back via the subscription', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row { sensor_id: number; temperature: number; label: string }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>(TEST_TOPIC, (row) => {
      received.push(row);
    });
    expect(subId).toBeTypeOf('bigint');

    // Reuse the server-supplied schema for publishing.
    const schema = (client as unknown as {
      subscriptions: Map<bigint, { schema: SchemaDescriptor }>
    }).subscriptions.get(subId)!.schema;

    const sent: Row[] = [
      { sensor_id: 1,   temperature: 23.5,   label: 'first'  },
      { sensor_id: 42,  temperature: -7.125, label: 'second' },
      { sensor_id: 999, temperature: 1.0e9,  label: 'third'  },
    ];
    for (const row of sent) {
      await client.publish(TEST_TOPIC, schema, row);
    }

    const deadline = Date.now() + 5_000;
    while (received.length < sent.length && Date.now() < deadline) {
      await new Promise((res) => setTimeout(res, 20));
    }

    expect(received).toHaveLength(sent.length);
    for (let i = 0; i < sent.length; ++i) {
      expect(received[i].sensor_id).toBe(sent[i].sensor_id);
      expect(received[i].temperature).toBeCloseTo(sent[i].temperature);
      expect(received[i].label).toBe(sent[i].label);
    }

    await client.unsubscribe(subId);
    client.close();
  });
});

// ---------------------------------------------------------------------
// Proto-generated SchemaDescriptor end-to-end. Verifies that
// protoc-gen-fletcher + the WebSocket transport agree across the
// whole stack: same proto fields → same SchemaDescriptor on both
// sides → same bytes on the wire → same decoded values back. The
// `protoc-gateway-client-ts` integration test proves byte-compat for
// the codec in isolation; this proves the proto-generated artefact
// also works against the live gateway.
// ---------------------------------------------------------------------
describe('protoc-gen-fletcher TS class over WebSocket', () => {
  it('publishes via TelemetrySchema and receives the same row back', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row { sensor_id: number; temperature: number; label: string }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>(TEST_TOPIC, (row) => {
      received.push(row);
    });

    // The server-supplied schema (from test-config.yml) and the
    // protoc-generated TelemetrySchema must describe the same fields
    // in the same order — otherwise the publish would land bytes the
    // server cannot interpret.
    const serverSchema = (client as unknown as {
      subscriptions: Map<bigint, { schema: SchemaDescriptor }>
    }).subscriptions.get(subId)!.schema;
    expect(TelemetrySchema.fields.map((f) => f.name)).toEqual(
      serverSchema.fields.map((f) => f.name),
    );
    expect(TelemetrySchema.fields.map((f) => f.wireType)).toEqual(
      serverSchema.fields.map((f) => f.wireType),
    );

    // Publish using the *proto-generated* schema, not the
    // server-supplied one. Proves the generated descriptor produces
    // bytes the gateway accepts and routes.
    const sent: Row = { sensor_id: 314, temperature: 42.0, label: 'from-protogen' };
    await client.publish(TEST_TOPIC, TelemetrySchema, sent);

    const deadline = Date.now() + 3_000;
    while (received.length === 0 && Date.now() < deadline) {
      await new Promise((res) => setTimeout(res, 20));
    }

    expect(received).toHaveLength(1);
    expect(received[0].sensor_id).toBe(sent.sensor_id);
    expect(received[0].temperature).toBeCloseTo(sent.temperature);
    expect(received[0].label).toBe(sent.label);

    await client.unsubscribe(subId);
    client.close();
  });
});

// ---------------------------------------------------------------------
// AC: binary frame layouts are exactly what the protocol documents:
//   server -> client:  [SUB_ID :8 LE][ENVELOPE :rest]
//   client -> server:  [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE :rest]
// We bypass the high-level client and inspect raw frame bytes on
// both directions.
// ---------------------------------------------------------------------
describe('binary frame layouts', () => {
  it('server -> client MESSAGE frame is [SUB_ID :8 LE][ENVELOPE]', async () => {
    const ws = new WebSocket(TEST_URL);
    ws.binaryType = 'arraybuffer';

    const { subId, rawBytes } = await new Promise<{ subId: bigint; rawBytes: Uint8Array }>(
      (res, rej) => {
        let capturedSubId: bigint | null = null;
        let schemaForPublish: SchemaDescriptor | null = null;
        ws.onopen = () => ws.send(buildSubscribe(TEST_TOPIC));
        ws.onmessage = (ev) => {
          if (typeof ev.data === 'string') {
            const parsed = parseTextResponse(ev.data);
            if (parsed.type === 'subscribed') {
              capturedSubId = parsed.subId;
              schemaForPublish = parsed.schema ?? null;
              // Publish on the same socket to trigger a MESSAGE frame
              // back to us via InProcessProvider's loopback.
              const row = encodePositional(schemaForPublish!, {
                sensor_id: 1, temperature: 1.0, label: 'frame-layout',
              });
              const env = serializeEnvelope({ row, attachments: new Map() });
              ws.send(buildPublish(TEST_TOPIC, env));
            } else if (parsed.type === 'error') {
              rej(new Error(parsed.message));
            }
          } else if (capturedSubId !== null) {
            const raw = new Uint8Array(ev.data as ArrayBuffer);
            res({ subId: capturedSubId, rawBytes: raw });
          }
        };
        ws.onerror = () => rej(new Error('ws error'));
      },
    );

    // Layout: first 8 bytes = sub_id LE.
    expect(rawBytes.byteLength).toBeGreaterThanOrEqual(8);
    const view = new DataView(rawBytes.buffer, rawBytes.byteOffset, rawBytes.byteLength);
    const wireSubId = view.getBigUint64(0, true);
    expect(wireSubId).toBe(subId);

    const parsed = parseBinaryMessage(rawBytes);
    expect(parsed.subId).toBe(subId);
    expect(parsed.envelope.byteLength).toBe(rawBytes.byteLength - 8);
    expect(parsed.envelope).toEqual(rawBytes.slice(8));

    ws.close();
  });

  it('client -> server PUBLISH frame is [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE]', () => {
    const topic = TEST_TOPIC;
    const envelopeBytes = serializeEnvelope({
      row: encodePositional(
        { fields: [{ name: 'x', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false }] },
        { x: 42 },
      ),
      attachments: new Map(),
    });

    const frame = buildPublish(topic, envelopeBytes);

    // First two bytes: topic length in LE uint16.
    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const topicLen = view.getUint16(0, true);
    expect(topicLen).toBe(topic.length);

    // Bytes 2..2+topicLen: topic name as UTF-8.
    const decoded = new TextDecoder().decode(frame.slice(2, 2 + topicLen));
    expect(decoded).toBe(topic);

    // Remaining bytes: envelope, byte-equal to the input.
    expect(frame.slice(2 + topicLen)).toEqual(envelopeBytes);
    expect(frame.byteLength).toBe(2 + topicLen + envelopeBytes.byteLength);
  });
});
