// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end integration test for the gateway's FastDDS provider.
//
// Topology: a C++ FastDDS peer and a TypeScript gateway-client both talk to
// the gateway, which is started with --provider fastdds on an isolated DDS
// domain. The test exercises both cross-language directions, covering all
// four pub/sub capabilities:
//
//   * C++ pub -> TS sub  (topic SensorFeed_CppToTs): the peer publishes a
//     known set of rows; the TS client subscribes through the gateway and
//     must receive them, decoded into the protoc-generated ISensorReading.
//   * TS pub -> C++ sub  (topic SensorFeed_TsToCpp): the TS client publishes
//     a row; the peer's subscriber receives it over DDS and echoes a
//     "RECV ..." line on stdout, which this suite asserts on.
//
// A third case proves the publisher-announced schema propagates the whole
// chain (FastDDS __schema -> gateway provider -> WebSocket subscribed frame).
// The gateway forwards a DDS schema in the subscribed frame only once it is
// available (the subscribe path is non-blocking and never waits on a
// publisher), so the case first lets the schema propagate via a held
// subscription, then asserts a fresh subscribe receives it synchronously.

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ChildProcess, spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FletcherClient, buildSubscribe } from 'fletcher-gateway-client';
import {
  SensorReading,
  SensorFeed_CppToTsTopic,
  SensorFeed_TsToCppTopic,
} from '../generated-ts/sensor_reading.fletcher.js';
import type { ISensorReading } from '../generated-ts/sensor_reading.fletcher.js';

const here = fileURLToPath(new URL('.', import.meta.url));

const TEST_PORT = parseInt(process.env.TEST_PORT ?? '19093', 10);
const TEST_URL = `ws://127.0.0.1:${TEST_PORT}`;
// Isolated, high DDS domain so this test does not cross-talk with
// pubsub-arrow-fastdds (137) on a shared --network host CI runner.
const DOMAIN_ID = process.env.DDS_DOMAIN_ID ?? '142';

// The rows the C++ peer publishes on CppToTs at startup (see fastdds_peer.cpp).
const EXPECTED_CPP_ROWS: ISensorReading[] = [
  { sensor_id: 1, value: 10.5, unit: 'u1' },
  { sensor_id: 2, value: 21.0, unit: 'u2' },
  { sensor_id: 3, value: 31.5, unit: 'u3' },
];

function findBinary(envVar: string, name: string, subdir?: string): string {
  const fromEnv = process.env[envVar];
  if (fromEnv) {
    return fromEnv;
  }
  const parts = subdir ? [subdir] : [];
  const candidates = [
    resolve(here, '..', 'build', 'Release', ...parts, name),
    resolve(here, '..', 'build', 'Release', ...parts, `${name}.exe`),
    resolve(here, '..', 'build', 'build', 'Release', ...parts, name),
  ];
  for (const c of candidates) {
    if (existsSync(c)) {
      return c;
    }
  }
  throw new Error(
    `${name} binary not found. Checked: ${candidates.join(', ')}. Set ${envVar} to override.`,
  );
}

// Spawn a child that prints "READY" on stdout once initialised, and exits on
// the stdin line "stop". `onLine` (optional) sees every stdout line so the
// caller can keep collecting output after READY (the peer's RECV lines).
async function spawnUntilReady(
  bin: string,
  args: string[],
  label: string,
  onLine?: (line: string) => void,
): Promise<ChildProcess> {
  const child = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  child.stderr?.on('data', (chunk) => process.stderr.write(`[${label} stderr] ${chunk}`));

  return new Promise<ChildProcess>((resolveFn, rejectFn) => {
    const rl = createInterface({ input: child.stdout! });
    const timeout = setTimeout(() => {
      if (!child.killed) {
        child.kill('SIGKILL');
      }
      rejectFn(new Error(`${label} did not print READY within 30 s`));
    }, 30_000);

    rl.on('line', (line) => {
      onLine?.(line);
      if (line.startsWith('READY')) {
        clearTimeout(timeout);
        resolveFn(child);
      }
    });
    child.on('error', (err) => {
      clearTimeout(timeout);
      if (!child.killed) {
        child.kill('SIGKILL');
      }
      rejectFn(err);
    });
    child.on('exit', (code) => {
      clearTimeout(timeout);
      rejectFn(new Error(`${label} exited before READY (code=${code})`));
    });
  });
}

async function stopChild(child: ChildProcess): Promise<void> {
  return new Promise<void>((resolveFn) => {
    const fallback = setTimeout(() => {
      if (!child.killed) {
        child.kill('SIGTERM');
      }
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

async function waitFor(predicate: () => boolean, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (!predicate() && Date.now() < deadline) {
    await new Promise((res) => setTimeout(res, 50));
  }
}

let gateway: ChildProcess;
let peer: ChildProcess;
// RECV lines collected from the C++ peer's stdout (TS -> C++ deliveries).
const recvLines: string[] = [];

beforeAll(async () => {
  gateway = await spawnUntilReady(
    findBinary('GATEWAY_BIN', 'gateway', 'gateway_build'),
    ['--provider', 'fastdds', '--domain-id', DOMAIN_ID, '--port', String(TEST_PORT), '--bind-address', '127.0.0.1'],
    'gateway',
  );
  peer = await spawnUntilReady(
    findBinary('FASTDDS_PEER_BIN', 'fastdds_peer'),
    ['--domain-id', DOMAIN_ID],
    'fastdds_peer',
    (line) => {
      if (line.startsWith('RECV ')) {
        recvLines.push(line);
      }
    },
  );
});

afterAll(async () => {
  if (peer) {
    await stopChild(peer);
  }
  if (gateway) {
    await stopChild(gateway);
  }
});

describe('gateway FastDDS provider — bidirectional', () => {
  it('C++ FastDDS peer publish -> TS subscribe through the gateway', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    const received: ISensorReading[] = [];
    const subId = await client.subscribe(SensorFeed_CppToTsTopic, SensorReading, (row) => {
      received.push(row);
    });

    // TRANSIENT_LOCAL + KEEP_ALL replays the peer's startup rows to the
    // gateway's late-joining DataReader, so order of subscribe vs publish
    // does not matter; allow headroom for cross-process DDS discovery.
    await waitFor(() => received.length >= EXPECTED_CPP_ROWS.length, 15_000);

    expect(received).toHaveLength(EXPECTED_CPP_ROWS.length);
    const byId = [...received].sort((a, b) => a.sensor_id - b.sensor_id);
    for (let i = 0; i < EXPECTED_CPP_ROWS.length; ++i) {
      expect(byId[i].sensor_id).toBe(EXPECTED_CPP_ROWS[i].sensor_id);
      expect(byId[i].value).toBeCloseTo(EXPECTED_CPP_ROWS[i].value);
      expect(byId[i].unit).toBe(EXPECTED_CPP_ROWS[i].unit);
    }

    await client.unsubscribe(subId);
    client.close();
  });

  it('TS publish -> C++ FastDDS peer subscribe through the gateway', async () => {
    const client = new FletcherClient({ url: TEST_URL });
    await client.connect();

    // Announce the schema so the gateway's FastDDS DataWriter + __schema
    // channel are established before publishing.
    await client.createTopic(SensorFeed_TsToCppTopic, SensorReading);

    const sent: ISensorReading = { sensor_id: 7, value: 1.5, unit: 'celsius' };
    await client.publish(SensorFeed_TsToCppTopic, SensorReading, sent);

    await waitFor(() => recvLines.some((l) => l.includes('sensor_id=7')), 15_000);

    const match = recvLines.find((l) => l.includes('sensor_id=7'));
    expect(match, `peer RECV lines: ${JSON.stringify(recvLines)}`).toBeDefined();
    expect(match).toContain('value=1.5');
    expect(match).toContain('unit=celsius');

    client.close();
  });

  it('gateway forwards the peer-announced schema once it has propagated', async () => {
    // Hold a typed subscription open so the gateway's (shared) Subscriber
    // retains the topic's schema future. Receiving a row proves the FastDDS
    // __schema reached the gateway provider and the future has resolved — the
    // provider buffers data until the schema is known, so a delivered row
    // implies a resolved schema.
    const keepAlive = new FletcherClient({ url: TEST_URL });
    await keepAlive.connect();
    let gotRow = false;
    const subId = await keepAlive.subscribe(SensorFeed_CppToTsTopic, SensorReading, () => {
      gotRow = true;
    });
    await waitFor(() => gotRow, 15_000);
    expect(gotRow, 'keep-alive subscription should receive a row first').toBe(true);

    // A fresh raw-WS subscribe to the same topic now gets the schema
    // synchronously in the subscribed response: the gateway forwards a DDS
    // schema whenever it is already available, which it now is (the shared
    // schema future is resolved). This is the FastDDS __schema -> gateway
    // provider -> WebSocket subscribed-frame propagation.
    const ws = new WebSocket(TEST_URL);
    ws.binaryType = 'arraybuffer';

    const response = await new Promise<Record<string, unknown>>((res, rej) => {
      const timeout = setTimeout(() => rej(new Error('no subscribed response within 15 s')), 15_000);
      ws.onopen = () => ws.send(buildSubscribe(SensorFeed_CppToTsTopic));
      ws.onmessage = (ev) => {
        if (typeof ev.data === 'string') {
          const j = JSON.parse(ev.data);
          if (j.type === 'subscribed') {
            clearTimeout(timeout);
            res(j);
          } else if (j.type === 'error') {
            clearTimeout(timeout);
            rej(new Error(j.message));
          }
        }
      };
      ws.onerror = () => {
        clearTimeout(timeout);
        rej(new Error('ws error'));
      };
    });

    expect(response.type).toBe('subscribed');
    expect(response.topic).toBe(SensorFeed_CppToTsTopic);
    const schema = response.schema as { fields: Array<{ name: string }> } | undefined;
    expect(schema, `subscribed response: ${JSON.stringify(response)}`).toBeDefined();
    expect(schema!.fields[0].name).toBe('sensor_id');

    ws.close();
    await keepAlive.unsubscribe(subId);
    keepAlive.close();
  });
});
