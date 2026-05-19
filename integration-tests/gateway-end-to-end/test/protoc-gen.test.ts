// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end tests that exercise the protoc-gen-fletcher → schema
// → WebSocket → gateway → loopback path. These tests intentionally
// depend on a generated `TelemetrySchema` (produced from
// `proto/telemetry.proto`) because they are the proof that the
// proto-gen toolchain produces a SchemaDescriptor that actually
// works against the live gateway.
//
// Protocol-level tests that do not touch protoc-gen live in
// `end-to-end.test.ts` to keep that file's coupling to generated
// artefacts to zero.
//
// The gateway is spawned on a different TCP port than
// `end-to-end.test.ts` so both files can run in parallel without
// fighting for the socket.

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ChildProcess, spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  FletcherClient,
  WireTypeId,
} from 'eiva-fletcher-gateway-client';
import { TelemetrySchema } from '../generated-ts/telemetry.fletcher.js';

const here = fileURLToPath(new URL('.', import.meta.url));

const TEST_PORT  = 19092;
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

async function spawnGateway(port: number): Promise<ChildProcess> {
  const bin = findGatewayBinary();
  const child = spawn(bin, [
    '--port', String(port),
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
  server = await spawnGateway(TEST_PORT);
});

afterAll(async () => {
  if (server) await stopGateway(server);
});

// ---------------------------------------------------------------------
// Sanity check: the generated TelemetrySchema is what the rest of the
// file expects. Catches any drift between proto/telemetry.proto and
// the assertions below before the end-to-end tests rely on it.
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
// The canonical happy-path round-trip. A client builds its schema
// from protoc-gen-fletcher → subscribes with that schema → publishes
// with it → receives the row back through the gateway's loopback.
// This is the "look here first" test for understanding how the
// system is meant to be used.
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
// Stress-variant of the happy path: three samples with distinct
// values across the three field types in TelemetrySchema. Verifies
// that ordering and value-fidelity hold across multiple publishes
// on the same subscription.
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
