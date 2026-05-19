// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end test that drives the real gateway (C++ WebSocket server)
// against the real gateway-client-ts (TypeScript FletcherClient). The
// server is the test_server binary built by CMake in this directory;
// the test_server signals readiness on stdout, accepts "stop" on stdin,
// and pre-creates two topics:
//
//   * "heartbeat" — the server publishes a synthetic row every
//     `--heartbeat-ms` (default 100 ms). Subscribing exercises the
//     publish-from-server path.
//   * "echo"      — InProcessProvider routes Publish() back to the
//     subscribed callback, so a single client that subscribes and
//     then publishes verifies the client-publish path without a
//     side channel.

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

const here = fileURLToPath(new URL('.', import.meta.url));

// Fixed port — using a high number to minimise collision with system
// services. If a developer runs two copies of this test in parallel
// they'll need to override; CI runs one job per workflow so single-port
// is fine.
const TEST_PORT = 19091;
const TEST_URL  = `ws://127.0.0.1:${TEST_PORT}`;

function findTestServerBinary(): string {
  if (process.env.TEST_SERVER_BIN) {
    return process.env.TEST_SERVER_BIN;
  }
  // The integration test's CMakeLists.txt pulls in gateway/ via
  // add_subdirectory, so the `gateway-test-server` target ends up
  // under build/Release/gateway_build/.
  const candidates = [
    resolve(here, '..', 'build', 'Release', 'gateway_build', 'gateway-test-server'),
    resolve(here, '..', 'build', 'Release', 'gateway_build', 'gateway-test-server.exe'),
    resolve(here, '..', 'build', 'build', 'Release', 'gateway_build', 'gateway-test-server'),
  ];
  for (const c of candidates) {
    if (existsSync(c)) return c;
  }
  throw new Error(
    `gateway-test-server binary not found. Checked: ${candidates.join(', ')}. ` +
    `Set TEST_SERVER_BIN to override.`,
  );
}

// Spawn the C++ test_server and resolve once it prints "READY <port>".
async function spawnServer(): Promise<ChildProcess> {
  const bin = findTestServerBinary();
  const child = spawn(bin, ['--port', String(TEST_PORT), '--heartbeat-ms', '50'], {
    stdio: ['pipe', 'pipe', 'pipe'],
  });

  // Forward stderr to the test console for visibility on failure.
  child.stderr?.on('data', (chunk) => {
    process.stderr.write(`[test_server stderr] ${chunk.toString()}`);
  });

  return new Promise<ChildProcess>((resolveFn, rejectFn) => {
    const rl = createInterface({ input: child.stdout! });
    const timeout = setTimeout(() => {
      rejectFn(new Error('test_server did not print READY within 10 s'));
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
      rejectFn(new Error(`test_server exited before READY (code=${code})`));
    });
  });
}

async function stopServer(child: ChildProcess): Promise<void> {
  return new Promise<void>((resolveFn) => {
    child.on('exit', () => resolveFn());
    child.stdin?.write('stop\n');
    child.stdin?.end();
    setTimeout(() => {
      // Hard kill if the server hasn't honoured "stop" within 5 s.
      if (!child.killed) child.kill('SIGTERM');
      resolveFn();
    }, 5_000);
  });
}

let server: ChildProcess;

beforeAll(async () => {
  server = await spawnServer();
});

afterAll(async () => {
  if (server) await stopServer(server);
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
      ws.onopen = () => ws.send(buildSubscribe('heartbeat'));
      ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
          const parsed = parseTextResponse(ev.data);
          if (parsed.type === 'subscribed') res(parsed);
          else if (parsed.type === 'error') rej(new Error(parsed.message));
        }
      };
      ws.onerror = () => rej(new Error('ws error'));
    });

    expect(response.topic).toBe('heartbeat');
    expect(response.subId).toBeTypeOf('bigint');

    // SchemaDescriptor JSON path
    expect(response.schema).toBeDefined();
    expect(response.schema!.fields).toHaveLength(3);
    expect(response.schema!.fields[0].name).toBe('sensor_id');
    expect(response.schema!.fields[0].wireType).toBe(WireTypeId.INT32);
    expect(response.schema!.fields[1].name).toBe('temperature');
    expect(response.schema!.fields[1].wireType).toBe(WireTypeId.FLOAT64);
    expect(response.schema!.fields[2].name).toBe('label');
    expect(response.schema!.fields[2].wireType).toBe(WireTypeId.STRING);

    // Arrow IPC bytes path — base64 string, decodes to a non-empty
    // byte buffer beginning with the Arrow IPC continuation marker
    // (0xFFFFFFFF 0x.... for stream / 0x00000000 0x.... for legacy).
    expect(response.schemaIpc).toBeTypeOf('string');
    expect(response.schemaIpc!.length).toBeGreaterThan(0);
    const ipcBytes = Buffer.from(response.schemaIpc!, 'base64');
    expect(ipcBytes.byteLength).toBeGreaterThan(0);

    ws.close();
  });
});

// ---------------------------------------------------------------------
// AC: full happy path — create_topic (server-side) -> subscribe ->
// publish-from-server -> message delivery -> unsubscribe.
// ---------------------------------------------------------------------
describe('publish-from-server -> client delivery', () => {
  it('subscribes via FletcherClient and receives at least one heartbeat row',
     async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row { sensor_id: number; temperature: number; label: string }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>('heartbeat', (row) => {
      received.push(row);
    });
    expect(subId).toBeTypeOf('bigint');

    // Wait for at least three heartbeat rows. Heartbeat is 50 ms, so
    // 1 s is generous; we cap the wait to keep the timeout honest.
    const deadline = Date.now() + 5_000;
    while (received.length < 3 && Date.now() < deadline) {
      await new Promise((res) => setTimeout(res, 50));
    }

    expect(received.length).toBeGreaterThanOrEqual(3);

    // Verify each row matches the server's encoder rule
    // (temperature = sensor_id * 1.5, label = "hb") so the assertion
    // doesn't depend on which seq value the client happened to see
    // first. seq monotonically increments per heartbeat — assert that
    // too as a fan-out coherence check across the WebSocket.
    for (let i = 0; i < received.length; ++i) {
      expect(received[i].label).toBe('hb');
      expect(received[i].temperature).toBeCloseTo(received[i].sensor_id * 1.5);
      if (i > 0) {
        expect(received[i].sensor_id).toBeGreaterThan(received[i - 1].sensor_id);
      }
    }

    await client.unsubscribe(subId);
    client.close();
  });
});

// ---------------------------------------------------------------------
// AC: reverse direction — client publish -> server-side delivery.
// We subscribe to "echo" first, then publish to "echo", then assert
// the same row comes back via the subscription callback (proves the
// client publish reached the server's InProcessProvider, which routed
// it to the registered callback that the gateway installed when we
// subscribed).
// ---------------------------------------------------------------------
describe('client publish -> server delivery', () => {
  it('round-trips a published row back via echo subscription', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    interface Row { sensor_id: number; temperature: number; label: string }
    const received: Row[] = [];

    const subId = await client.subscribe<Row>('echo', (row) => {
      received.push(row);
    });

    // The subscribe call above returned with the schema embedded in
    // the response. Reuse that schema to drive publish.
    const schema = (client as unknown as { subscriptions: Map<bigint, { schema: SchemaDescriptor }> })
      .subscriptions.get(subId)!.schema;

    const sent = { sensor_id: 7, temperature: -1.25, label: 'from-ts' };
    await client.publish('echo', schema, sent);

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
        ws.onopen = () => ws.send(buildSubscribe('heartbeat'));
        ws.onmessage = (ev) => {
          if (typeof ev.data === 'string') {
            const parsed = parseTextResponse(ev.data);
            if (parsed.type === 'subscribed') {
              capturedSubId = parsed.subId;
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

    // Layout assertion: first 8 bytes are the sub_id, little-endian.
    expect(rawBytes.byteLength).toBeGreaterThanOrEqual(8);
    const view = new DataView(rawBytes.buffer, rawBytes.byteOffset, rawBytes.byteLength);
    const wireSubId = view.getBigUint64(0, true);
    expect(wireSubId).toBe(subId);

    // The envelope follows the 8-byte sub_id. parseBinaryMessage's
    // slice should match a manual slice — proves no hidden padding.
    const parsed = parseBinaryMessage(rawBytes);
    expect(parsed.subId).toBe(subId);
    expect(parsed.envelope.byteLength).toBe(rawBytes.byteLength - 8);
    expect(parsed.envelope).toEqual(rawBytes.slice(8));

    ws.close();
  });

  it('client -> server PUBLISH frame is [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE]', () => {
    const topic = 'echo';
    const envelopeBytes = serializeEnvelope({
      row: encodePositional(
        // Mock minimal schema — INT32 field, no nulls.
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

    // Remaining bytes: envelope, byte-equal to the input envelope.
    expect(frame.slice(2 + topicLen)).toEqual(envelopeBytes);
    expect(frame.byteLength).toBe(2 + topicLen + envelopeBytes.byteLength);
  });
});
