// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Gateway protocol tests, with no dependency on protoc-gen-fletcher.
// Schemas here are either irrelevant (raw routing assertions) or
// constructed by hand inside the test to keep the file
// self-contained. The protoc-gen happy path lives in
// `protoc-gen.test.ts`.

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
} from 'fletcher-gateway-client';
import type { SchemaDescriptor } from 'fletcher-gateway-client';

const here = fileURLToPath(new URL('.', import.meta.url));

// Port can be overridden via env (e.g. when running two copies of the
// suite in parallel or when 19091 is occupied locally).
const TEST_PORT = parseInt(process.env.TEST_PORT ?? '19091', 10);
const TEST_URL = `ws://127.0.0.1:${TEST_PORT}`;
const TEST_TOPIC = 'protocol';

// Hand-constructed schema — keeps this file independent of any
// generated TS file. Mirrors what a typical proto-gen output would
// look like (int32 / float64 / utf8) so the round-trip test below
// exercises null bitfield, fixed-width, and variable-length encodings
// without depending on protoc-gen-fletcher.
const HAND_BUILT_SCHEMA: SchemaDescriptor = {
  fields: [
    { name: 'sensor_id', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false },
    { name: 'temperature', fieldNumber: 2, wireType: WireTypeId.FLOAT64, nullable: false },
    { name: 'label', fieldNumber: 3, wireType: WireTypeId.STRING, nullable: false },
  ],
};

// Single-field shorthand for the binary-frame-layout assertions, which
// only need ANY valid schema to construct one envelope.
const MINIMAL_SCHEMA: SchemaDescriptor = {
  fields: [{ name: 'x', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false }],
};

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

async function spawnGateway(port: number): Promise<ChildProcess> {
  const bin = findGatewayBinary();
  const child = spawn(bin, ['--port', String(port), '--bind-address', '127.0.0.1'], {
    stdio: ['pipe', 'pipe', 'pipe'],
  });

  child.stderr?.on('data', (chunk) => {
    process.stderr.write(`[gateway stderr] ${chunk.toString()}`);
  });

  return new Promise<ChildProcess>((resolveFn, rejectFn) => {
    const rl = createInterface({ input: child.stdout! });
    // If the child spawns but never prints READY, kill it explicitly
    // before rejecting so we don't leave a stray process around when
    // afterAll has no `server` reference to clean up.
    const timeout = setTimeout(() => {
      if (!child.killed) child.kill('SIGKILL');
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
      if (!child.killed) child.kill('SIGKILL');
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
    // Cooperative shutdown: write "stop" to stdin and wait for the
    // process to exit. The fallback timer escalates to SIGTERM if
    // the child doesn't honour "stop" within 5 s; we clear it on
    // clean exit so Vitest can shut down immediately instead of
    // sitting on a live timer for the full 5 s.
    const fallback = setTimeout(() => {
      if (!child.killed) child.kill('SIGTERM');
      resolveFn();
    }, 5_000);
    child.on('exit', () => {
      clearTimeout(fallback);
      resolveFn();
    });
    child.stdin?.write('stop\n');
    child.stdin?.end();
  });
}

let server: ChildProcess;

beforeAll(async () => {
  server = await spawnGateway(TEST_PORT);
});

afterAll(async () => {
  if (server) await stopGateway(server);
});

// ---------------------------------------------------------------------
// subscribed text response carries routing only (subId + topic); no
// schema, no schemaIpc. Locks in the schema-agnostic gateway contract.
// ---------------------------------------------------------------------
describe('subscribed response — routing only', () => {
  it('subscribed text frame contains subId + topic and nothing else', async () => {
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
// FletcherClient round-trip without protoc-gen — verifies the basic
// publish ↔ subscription path with a hand-built schema. The
// protoc-gen.test.ts file has the same shape using the generated
// TelemetrySchema; this one stays here so it is impossible to break
// the high-level client API without breaking either of these two
// independent tests.
// ---------------------------------------------------------------------
describe('client publish ↔ subscription round-trip', () => {
  it('delivers multiple distinct published rows back via the subscription', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row {
      sensor_id: number;
      temperature: number;
      label: string;
    }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>(TEST_TOPIC, HAND_BUILT_SCHEMA, (row) => {
      received.push(row);
    });

    const sent: Row[] = [
      { sensor_id: 1, temperature: 23.5, label: 'first' },
      { sensor_id: 42, temperature: -7.125, label: 'second' },
      { sensor_id: 999, temperature: 1.0e9, label: 'third' },
    ];
    for (const row of sent) {
      await client.publish(TEST_TOPIC, HAND_BUILT_SCHEMA, row);
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
// Gateway-supplied schema path. A publisher announces a schema via
// `createTopic`; later subscribers can either fetch that schema from
// the gateway's `subscribed` response (raw WS, asserted explicitly
// below) or let FletcherClient pick it up implicitly (second test).
//
// Why schema delivery matters at all: Fletcher's row wire format is
// positional ([null_bitfield][f0_bytes][f1_bytes][...] with no field
// tags), so a subscriber that wants to decode bytes into a structured
// object has to know the field list and types. That information has
// to reach the subscriber by some channel; the gateway-supplied
// schema is one of two supported channels (the other being a
// client-supplied SchemaDescriptor passed to `subscribe`).
// ---------------------------------------------------------------------
describe('subscriber gets schema from gateway', () => {
  it('gateway forwards publisher-announced schema in the subscribed response', async () => {
    const PUB_TOPIC = 'protocol/gateway-supplied-schema';

    // Publisher: announce the schema. Gateway caches it per topic.
    const pub = new FletcherClient({ url: TEST_URL });
    await pub.connect();
    await pub.createTopic(PUB_TOPIC, HAND_BUILT_SCHEMA);

    // Subscriber: connect raw and inspect the subscribed response
    // directly, so the assertion proves the schema came from the
    // server rather than relying on FletcherClient's internal
    // bookkeeping.
    const ws = new WebSocket(TEST_URL);
    ws.binaryType = 'arraybuffer';
    const response = await new Promise<Record<string, unknown>>((res, rej) => {
      ws.onopen = () => ws.send(buildSubscribe(PUB_TOPIC));
      ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
          const j = JSON.parse(ev.data);
          if (j.type === 'subscribed') res(j);
          else if (j.type === 'error') rej(new Error(j.message));
        }
      };
      ws.onerror = () => rej(new Error('ws error'));
    });

    // Routing fields.
    expect(response.type).toBe('subscribed');
    expect(response.topic).toBe(PUB_TOPIC);
    expect(typeof response.subId).toBe('string');

    // Schema fields — the whole point of this test. Both
    // representations the gateway emits should be present and
    // structurally aligned with what the publisher announced.
    const schema = response.schema as {
      fields: Array<{ name: string; wireType: number; nullable: boolean }>;
    };
    expect(schema).toBeDefined();
    expect(schema.fields).toHaveLength(3);
    expect(schema.fields[0].name).toBe('sensor_id');
    expect(schema.fields[0].wireType).toBe(WireTypeId.INT32);
    expect(schema.fields[1].name).toBe('temperature');
    expect(schema.fields[1].wireType).toBe(WireTypeId.FLOAT64);
    expect(schema.fields[2].name).toBe('label');
    expect(schema.fields[2].wireType).toBe(WireTypeId.STRING);

    // Arrow IPC representation — present, base64, decodes to a
    // non-empty buffer. Clients that prefer to work with Arrow JS
    // would use this path; ObjectBackend uses the `schema` field
    // above.
    expect(response.schemaIpc).toBeTypeOf('string');
    const ipcBytes = Buffer.from(response.schemaIpc as string, 'base64');
    expect(ipcBytes.byteLength).toBeGreaterThan(0);

    ws.close();
    pub.close();
  });

  it('FletcherClient subscribe(topic, cb) uses the gateway-supplied schema', async () => {
    const PUB_TOPIC = 'protocol/gateway-supplied-roundtrip';

    const pub = new FletcherClient({ url: TEST_URL });
    await pub.connect();
    await pub.createTopic(PUB_TOPIC, HAND_BUILT_SCHEMA);

    const sub = new FletcherClient({ url: TEST_URL });
    await sub.connect();

    interface Row {
      sensor_id: number;
      temperature: number;
      label: string;
    }
    const received: Row[] = [];
    // No schema argument — FletcherClient must fall back to the
    // schema the gateway hands back in the subscribed response.
    const subId = await sub.subscribe<Row>(PUB_TOPIC, (row) => {
      received.push(row);
    });

    const sent: Row = { sensor_id: 17, temperature: 3.14, label: 'gateway-fwd' };
    await pub.publish(PUB_TOPIC, HAND_BUILT_SCHEMA, sent);

    const deadline = Date.now() + 3_000;
    while (received.length === 0 && Date.now() < deadline) {
      await new Promise((res) => setTimeout(res, 20));
    }

    expect(received).toHaveLength(1);
    expect(received[0].sensor_id).toBe(sent.sensor_id);
    expect(received[0].temperature).toBeCloseTo(sent.temperature);
    expect(received[0].label).toBe(sent.label);

    await sub.unsubscribe(subId);
    sub.close();
    pub.close();
  });
});

// ---------------------------------------------------------------------
// Binary frame layouts are exactly what the protocol documents:
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
              // Publish on the same socket to trigger a loopback
              // delivery and give the test a MESSAGE frame to
              // inspect. Use the file's minimal hand-built schema
              // so the test is self-contained.
              const row = encodePositional(MINIMAL_SCHEMA, { x: 42 });
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
      row: encodePositional(MINIMAL_SCHEMA, { x: 42 }),
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
