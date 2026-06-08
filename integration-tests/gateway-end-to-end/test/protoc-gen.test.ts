// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end tests that exercise the protoc-gen-fletcher → schema →
// WebSocket → gateway → loopback path. The proto generator emits two
// artefacts per message:
//
//   * `ITelemetry` — the row interface (TypeScript type only).
//   * `Telemetry`  — a `TypedSchema<ITelemetry>` runtime const that
//                    bundles the SchemaDescriptor with a phantom
//                    `ITelemetry` binding.
//
// Passing `Telemetry` to `FletcherClient.publish` / `subscribe` lets
// TypeScript infer `ITelemetry` for the data argument / callback row
// parameter — call sites never name the row type by hand.
//
// Protocol-level tests that do not touch protoc-gen live in
// `end-to-end.test.ts`. Each provider runs as its own test context
// (see describe.each) on its own TCP port, and both files use distinct
// ports so they can run in parallel without fighting for a socket.

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { ChildProcess, spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { FletcherClient, WireTypeId } from 'fletcher-gateway-client';
import { Telemetry } from '../generated-ts/telemetry.fletcher.js';
import type { ITelemetry } from '../generated-ts/telemetry.fletcher.js';

const here = fileURLToPath(new URL('.', import.meta.url));

// Port can be overridden via env (e.g. when 19092 is occupied locally).
const TEST_PORT = parseInt(process.env.TEST_PORT ?? '19092', 10);
const TEST_TOPIC = 'telemetry';

// The gateway behaves identically over either provider; the WebSocket happy
// paths below run once per provider as a separate test context (see
// describe.each), so a single `npm test` proves it. FastDDS gets its own port,
// an isolated high domain id, and a more generous round-trip deadline because
// it delivers over DDS (discovery + intra-participant transport) rather than a
// direct loopback.
interface ProviderConfig {
  name: string;
  port: number;
  domainId?: string;
  roundtripMs: number;
}
const PROVIDERS: ProviderConfig[] = [
  { name: 'inprocess', port: TEST_PORT, roundtripMs: 5_000 },
  {
    name: 'fastdds',
    port: TEST_PORT + 3,
    domainId: process.env.DDS_DOMAIN_ID ?? '152',
    roundtripMs: 15_000,
  },
];

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

async function spawnGateway(cfg: ProviderConfig): Promise<ChildProcess> {
  const bin = findGatewayBinary();
  const args = ['--port', String(cfg.port), '--bind-address', '127.0.0.1', '--provider', cfg.name];
  if (cfg.domainId) {
    args.push('--domain-id', cfg.domainId);
  }
  const child = spawn(bin, args, {
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
    // Cooperative shutdown: write "stop" to stdin and wait for exit.
    // Clear the fallback timer on clean exit so Vitest can shut down
    // immediately instead of sitting on a live timer for 5 s.
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

// ---------------------------------------------------------------------
// Sanity check on the generator output. Catches drift between
// proto/telemetry.proto and the assertions below before the rest of
// the suite leans on it. Pure codegen check — no gateway, so it lives
// outside the per-provider contexts.
// ---------------------------------------------------------------------
describe('generated Telemetry', () => {
  it('has the three expected fields with the right wire types', () => {
    expect(Telemetry.fields).toHaveLength(3);
    expect(Telemetry.fields[0].name).toBe('sensor_id');
    expect(Telemetry.fields[0].wireType).toBe(WireTypeId.INT32);
    expect(Telemetry.fields[1].name).toBe('temperature');
    expect(Telemetry.fields[1].wireType).toBe(WireTypeId.FLOAT64);
    expect(Telemetry.fields[2].name).toBe('label');
    expect(Telemetry.fields[2].wireType).toBe(WireTypeId.STRING);
  });
});

// Each provider is its own test context, so `npm test` runs every WebSocket
// case below against both the in-process and FastDDS providers.
describe.each(PROVIDERS)('gateway over $name provider', (cfg) => {
  const gatewayUrl = `ws://127.0.0.1:${cfg.port}`;
  const roundtripMs = cfg.roundtripMs;
  let server: ChildProcess;

  beforeAll(async () => {
    server = await spawnGateway(cfg);
  });

  afterAll(async () => {
    if (server) await stopGateway(server);
  });

  // -------------------------------------------------------------------
  // Canonical typed happy-path. Pass `Telemetry` to subscribe and
  // publish; TypeScript infers `ITelemetry` for both the callback row
  // parameter and the data argument — no `<T>` generic, no `: Row`
  // interface declared at the call site. The data literal would fail
  // to type-check if it diverged from `ITelemetry`.
  // -------------------------------------------------------------------
  describe('protoc-gen-fletcher TS class over WebSocket', () => {
    it('subscribe + publish using Telemetry are typed end-to-end', async () => {
      const client = new FletcherClient({ url: gatewayUrl });
      await client.connect();

      // Announce the schema so the FastDDS provider can deliver (it buffers
      // samples until a schema is known); the in-process provider ignores it.
      await client.createTopic(TEST_TOPIC, Telemetry);

      const received: ITelemetry[] = [];
      const subId = await client.subscribe(
        TEST_TOPIC,
        Telemetry,
        (row) => {
          received.push(row);
        }, // row: ITelemetry inferred
      );

      const sent: ITelemetry = {
        sensor_id: 314,
        temperature: 42.0,
        label: 'from-protogen',
      };
      await client.publish(TEST_TOPIC, Telemetry, sent);

      const deadline = Date.now() + roundtripMs;
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

  // -------------------------------------------------------------------
  // Stress-variant of the happy path: three samples with distinct
  // values across the three field types. Verifies ordering and value
  // fidelity across multiple publishes on the same subscription, still
  // fully typed via `Telemetry`.
  // -------------------------------------------------------------------
  describe('client publish ↔ subscription round-trip', () => {
    it('delivers multiple distinct published rows back via the subscription', async () => {
      const ROUNDTRIP_TOPIC = 'telemetry/roundtrip';
      const client = new FletcherClient({ url: gatewayUrl });
      await client.connect();

      await client.createTopic(ROUNDTRIP_TOPIC, Telemetry);

      const received: ITelemetry[] = [];
      const subId = await client.subscribe(ROUNDTRIP_TOPIC, Telemetry, (row) => {
        received.push(row);
      });

      const sent: ITelemetry[] = [
        { sensor_id: 1, temperature: 23.5, label: 'first' },
        { sensor_id: 42, temperature: -7.125, label: 'second' },
        { sensor_id: 999, temperature: 1.0e9, label: 'third' },
      ];
      for (const row of sent) {
        await client.publish(ROUNDTRIP_TOPIC, Telemetry, row);
      }

      const deadline = Date.now() + roundtripMs;
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

  // -------------------------------------------------------------------
  // Gateway-supplied schema path: a publisher announces `Telemetry` via
  // createTopic, then a second connection subscribes WITHOUT supplying
  // a schema and still gets typed delivery — the gateway forwards the
  // schema to the subscriber in the subscribed response, and the
  // caller asserts the row type via the generic parameter on subscribe.
  //
  // Demonstrates the other half of the "schema is the client's
  // contract" story: clients can either bring their own schema (above)
  // or rely on a publisher to have announced one.
  // -------------------------------------------------------------------
  describe('subscribe<ITelemetry> with gateway-supplied schema', () => {
    it('subscriber gets typed delivery without passing a local schema', async () => {
      const PUB_TOPIC = 'telemetry/gateway-supplied';

      const pub = new FletcherClient({ url: gatewayUrl });
      await pub.connect();
      // Announce the schema once; gateway will forward it to future
      // subscribers in their subscribed response.
      await pub.createTopic(PUB_TOPIC, Telemetry);

      const sub = new FletcherClient({ url: gatewayUrl });
      await sub.connect();

      const received: ITelemetry[] = [];
      const subId = await sub.subscribe<ITelemetry>(PUB_TOPIC, (row) => {
        received.push(row);
      });

      const sent: ITelemetry = {
        sensor_id: 1,
        temperature: 99.9,
        label: 'gateway-forwarded',
      };
      await pub.publish(PUB_TOPIC, Telemetry, sent);

      const deadline = Date.now() + roundtripMs;
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
});
