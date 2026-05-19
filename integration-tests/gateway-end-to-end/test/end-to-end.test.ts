// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end test that drives the real `gateway` exe (C++ WebSocket
// server) against the real gateway-client-ts (TypeScript
// FletcherClient).
//
// The gateway is schema-agnostic — it knows nothing about topic
// schemas or which topics will exist. The test owns the schema via
// `TelemetrySchema` generated from `proto/telemetry.proto` by
// `protoc-gen-fletcher` and passes it explicitly to both subscribe
// and publish. Topics are created implicitly on first subscribe; no
// pre-declaration on the server side.

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
import { TelemetrySchema } from '../generated-ts/telemetry.fletcher.js';

const here = fileURLToPath(new URL('.', import.meta.url));

const TEST_PORT  = 19091;
const TEST_URL   = `ws://127.0.0.1:${TEST_PORT}`;
const TEST_TOPIC = 'telemetry';

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

async function spawnGateway(): Promise<ChildProcess> {
  const bin = findGatewayBinary();
  const child = spawn(bin, [
    '--port', String(TEST_PORT),
    '--bind-address', '127.0.0.1',
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
// Sanity check: the proto-generated TelemetrySchema is what the test
// expects. Catches any drift between proto/telemetry.proto and the
// hardcoded scenario values below before the more end-to-end tests
// run.
// ---------------------------------------------------------------------
describe('generated TelemetrySchema', () => {
  it('has the three expected fields with the right wire types', () => {
    expect(TelemetrySchema.fields).toHaveLength(3);
    expect(TelemetrySchema.fields[0].name).toBe('sensor_id');
    expect(TelemetrySchema.fields[0].wireType).toBe(WireTypeId.INT32);
    expect(TelemetrySchema.fields[1].name).toBe('temperature');
    expect(TelemetrySchema.fields[1].wireType).toBe(WireTypeId.FLOAT64);
    expect(TelemetrySchema.fields[2].name).toBe('label');
    expect(TelemetrySchema.fields[2].wireType).toBe(WireTypeId.STRING);
  });
});

// ---------------------------------------------------------------------
// subscribed text response now carries routing only (subId + topic);
// no schema, no schemaIpc. The gateway is schema-agnostic.
// ---------------------------------------------------------------------
describe('subscribed response — routing only', () => {
  it('subscribed text frame contains subId + topic and nothing else',
     async () => {
    const ws = new WebSocket(TEST_URL);
    ws.binaryType = 'arraybuffer';

    const raw = await new Promise<Record<string, unknown>>((res, rej) => {
      ws.onopen = () => ws.send(buildSubscribe(TEST_TOPIC));
      ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
          const j = JSON.parse(ev.data);
          if (j.type === 'subscribed') res(j);
          else if (j.type === 'error') rej(new Error(j.message));
        }
      };
      ws.onerror = () => rej(new Error('ws error'));
    });

    expect(raw.type).toBe('subscribed');
    expect(raw.topic).toBe(TEST_TOPIC);
    expect(typeof raw.subId).toBe('string');
    // Absence assertions: schema-handling has moved fully to the
    // client side.
    expect(raw.schema).toBeUndefined();
    expect(raw.schemaIpc).toBeUndefined();

    ws.close();
  });
});

// ---------------------------------------------------------------------
// Client publish ↔ subscription round-trip using the generated
// TelemetrySchema. Exercises both directions of the protocol with
// the schema source-of-truth the client actually owns.
// ---------------------------------------------------------------------
describe('client publish ↔ subscription round-trip', () => {
  it('delivers multiple distinct published rows back via the subscription', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row { sensor_id: number; temperature: number; label: string }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>(
      TEST_TOPIC,
      TelemetrySchema,
      (row) => { received.push(row); },
    );
    expect(subId).toBeTypeOf('bigint');

    const sent: Row[] = [
      { sensor_id: 1,   temperature: 23.5,   label: 'first'  },
      { sensor_id: 42,  temperature: -7.125, label: 'second' },
      { sensor_id: 999, temperature: 1.0e9,  label: 'third'  },
    ];
    for (const row of sent) {
      await client.publish(TEST_TOPIC, TelemetrySchema, row);
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
// The happy-path story end-to-end: a client builds its schema from
// protoc-gen-fletcher → subscribes with that schema → publishes with
// it → receives the row back through the gateway's loopback. Since
// the gateway is schema-agnostic, this is the canonical way to use
// the system. The "round-trip" test above stress-tests the path with
// three samples; this one highlights the architecture in a single
// minimal round-trip and is the place to look when reading the test
// suite to understand how the pieces fit together.
// ---------------------------------------------------------------------
describe('protoc-gen-fletcher TS class over WebSocket', () => {
  it('subscribe + publish using TelemetrySchema work end-to-end', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row { sensor_id: number; temperature: number; label: string }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>(
      TEST_TOPIC,
      TelemetrySchema,
      (row) => { received.push(row); },
    );

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
// ---------------------------------------------------------------------
describe('binary frame layouts', () => {
  it('server -> client MESSAGE frame is [SUB_ID :8 LE][ENVELOPE]', async () => {
    const ws = new WebSocket(TEST_URL);
    ws.binaryType = 'arraybuffer';

    const { subId, rawBytes } = await new Promise<{ subId: bigint; rawBytes: Uint8Array }>(
      (res, rej) => {
        let capturedSubId: bigint | null = null;
        ws.onopen = () => ws.send(buildSubscribe(TEST_TOPIC));
        ws.onmessage = (ev) => {
          if (typeof ev.data === 'string') {
            const parsed = parseTextResponse(ev.data);
            if (parsed.type === 'subscribed') {
              capturedSubId = parsed.subId;
              // Publish on the same socket using TelemetrySchema —
              // triggers the loopback delivery to our subscription
              // and gives the test the MESSAGE frame to inspect.
              const row = encodePositional(TelemetrySchema, {
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

    const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
    const topicLen = view.getUint16(0, true);
    expect(topicLen).toBe(topic.length);

    const decoded = new TextDecoder().decode(frame.slice(2, 2 + topicLen));
    expect(decoded).toBe(topic);

    expect(frame.slice(2 + topicLen)).toEqual(envelopeBytes);
    expect(frame.byteLength).toBe(2 + topicLen + envelopeBytes.byteLength);
  });
});
