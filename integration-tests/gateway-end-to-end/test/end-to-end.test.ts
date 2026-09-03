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
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
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
} from '@eiva/fletcher-gateway-client';
import type { SchemaDescriptor } from '@eiva/fletcher-gateway-client';
import { findBinaryRecursive } from './find-binary.js';

const here = fileURLToPath(new URL('.', import.meta.url));

// Port can be overridden via env (e.g. when running two copies of the
// suite in parallel or when 19091 is occupied locally).
const TEST_PORT = parseInt(process.env.TEST_PORT ?? '19091', 10);
const TEST_TOPIC = 'protocol';

// The gateway protocol must behave identically regardless of pub/sub provider.
// The whole suite runs once per provider as a separate test context (see
// describe.each below), so a single `npm test` proves provider-agnostic
// behaviour. FastDDS gets its own port and an isolated high domain id to avoid
// cross-talk, and a more generous round-trip deadline because it delivers over
// DDS (discovery + intra-participant transport) rather than a direct loopback.
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
    domainId: process.env.DDS_DOMAIN_ID ?? '151',
    roundtripMs: 15_000,
  },
];

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
  const buildDir = resolve(here, '..', 'build');
  const name = process.platform === 'win32' ? 'gateway.exe' : 'gateway';
  const found = findBinaryRecursive(buildDir, name);
  if (found) return found;
  throw new Error(
    `gateway binary (${name}) not found under ${buildDir}. ` + `Set GATEWAY_BIN to override.`,
  );
}

async function spawnGateway(cfg: ProviderConfig, extraArgs: string[] = []): Promise<ChildProcess> {
  const bin = findGatewayBinary();
  const args = ['--port', String(cfg.port), '--bind-address', '127.0.0.1', '--provider', cfg.name];
  if (cfg.domainId) {
    args.push('--domain-id', cfg.domainId);
  }
  args.push(...extraArgs);
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

// Spawn the gateway and collect its exit code plus everything it wrote to
// stderr, for the refusal cases below — neither refusal ever prints READY, so
// spawnGateway's READY-polling promise is the wrong shape for them.
async function spawnGatewayExpectingExit(
  args: string[],
): Promise<{ code: number | null; stderr: string }> {
  const bin = findGatewayBinary();
  const child = spawn(bin, args, { stdio: ['pipe', 'pipe', 'pipe'] });
  let stderr = '';
  child.stderr?.on('data', (chunk) => {
    stderr += chunk.toString();
  });
  const code = await new Promise<number | null>((res) => {
    // A refusal case that stops refusing does not hang the suite for the full test timeout: the
    // watchdog kills a gateway that came up anyway, so the assertion below fails on the exit code
    // with the stderr in hand rather than on a 30 s timeout with nothing.
    const watchdog = setTimeout(() => {
      if (!child.killed) child.kill('SIGKILL');
    }, 10_000);
    child.on('exit', (c) => {
      clearTimeout(watchdog);
      res(c);
    });
  });
  return { code, stderr };
}

// ---------------------------------------------------------------------
// PDA-DEC-5: the gateway's provider selection, PROVED not just asserted.
//
// "The gateway still works" is unfalsifiable against a stale binary: `npm
// test` runs whatever gateway.exe already sits in build/, so a binary built
// before this stage would pass the whole describe.each battery below
// unchanged. The two refusal cases here are staleness DETECTORS, not just
// assertions: the pre-PDA-DEC-5 binary refuses both an unknown name and a
// path-shaped value through the SAME branch and the SAME message
// (`unknown provider: ... (expected inprocess|fastdds)`), so neither case can
// pass against it. Own port for the READY case (DEBT-2): TEST_PORT and
// TEST_PORT + 3 are held by the two describe.each contexts below.
// ---------------------------------------------------------------------
describe('provider selection', () => {
  it('an unregistered --provider name exits 2 naming what IS registered', async () => {
    const { code, stderr } = await spawnGatewayExpectingExit([
      '--port',
      String(TEST_PORT + 6),
      '--bind-address',
      '127.0.0.1',
      '--provider',
      'bogus',
    ]);
    expect(code).toBe(2);
    // Wording the OLD binary cannot emit: it says "unknown provider: bogus
    // (expected inprocess|fastdds)", which contains neither "available:" nor
    // "no built-in provider named" — pinned to the registry's own phrasing
    // (provider_registry.cpp), not to "names inprocess and fastdds somewhere",
    // which the old message also satisfies.
    expect(stderr).toContain('no built-in provider named');
    expect(stderr).toContain('available:');
  });

  it('a path-shaped --provider value exits 2 saying this build cannot load drivers', async () => {
    const { code, stderr } = await spawnGatewayExpectingExit([
      '--port',
      String(TEST_PORT + 7),
      '--bind-address',
      '127.0.0.1',
      '--provider',
      './nope.so',
    ]);
    expect(code).toBe(2);
    // The OLD binary's ONLY refusal message is "unknown provider: ... (expected
    // inprocess|fastdds)", which never says "cannot load drivers" — this is the
    // one case that cannot pass against any pre-PDA-DEC-5 gateway.exe.
    expect(stderr).toContain('cannot load drivers');
  });

  it('--provider inprocess still resolves and prints READY', async () => {
    const child = await spawnGateway({
      name: 'inprocess',
      port: TEST_PORT + 8,
      roundtripMs: 5_000,
    });
    await stopGateway(child);
  });
});

// ---------------------------------------------------------------------
// PDA-DEC-6 fix cycle 1 (review 4b S1/S2): `--provider-config` is the ONLY
// route by which the owner's charter requirement (b) - configure the driver
// with protocol-specific setup at run time - is reachable from gateway.exe,
// and nothing tested it. A regression here is silent in exactly the way that
// matters: drop `config.document = args.document` and the gateway still
// starts, still serves, and quietly runs on the provider's defaults.
//
// The property case is the one that proves the BYTES ARRIVE: the message it
// asserts is the Fast DDS provider's own, so it cannot be produced by a
// gateway that reads the file and forgets to forward it.
// ---------------------------------------------------------------------
describe('provider configuration', () => {
  let dir: string;

  beforeAll(() => {
    dir = mkdtempSync(join(tmpdir(), 'fletcher-provider-config-'));
  });

  afterAll(() => {
    rmSync(dir, { recursive: true, force: true });
  });

  function write(name: string, contents: string): string {
    const path = join(dir, name);
    writeFileSync(path, contents);
    return path;
  }

  const ANCHOR_ONLY = [
    '<?xml version="1.0" encoding="UTF-8"?>',
    '<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">',
    '  <profiles>',
    '    <participant profile_name="fletcher_participant">',
    '      <rtps>',
    '        <propertiesPolicy>',
    '          <properties>',
    '            <property><name>PROPERTY_NAME</name><value>131072</value></property>',
    '          </properties>',
    '        </propertiesPolicy>',
    '      </rtps>',
    '    </participant>',
    '  </profiles>',
    '</dds>',
  ].join('\n');

  it('a missing --provider-config file exits 2', async () => {
    const { code, stderr } = await spawnGatewayExpectingExit([
      '--port',
      String(TEST_PORT + 10),
      '--bind-address',
      '127.0.0.1',
      '--provider-config',
      join(dir, 'does-not-exist.xml'),
    ]);
    expect(code).toBe(2);
    expect(stderr).toContain('cannot read --provider-config');
  });

  it('an unreadable --provider-config file exits 2', async () => {
    // A DIRECTORY. Windows refuses it at the open; on Linux open(2) on a directory SUCCEEDS and
    // only the read fails (EISDIR), so the gateway refuses a directory explicitly instead of
    // letting a zero-byte read masquerade as an empty file. Either wording in the "unreadable"
    // family is accepted here, because which of the two the gateway reaches is an implementation
    // detail; the two things that must NOT happen are pinned below - it must not fall through to
    // "unconfigured", and it must not borrow the EMPTY-file wording, which is a different
    // condition with a different remedy and has its own case underneath this one.
    const { code, stderr } = await spawnGatewayExpectingExit([
      '--port',
      String(TEST_PORT + 10),
      '--bind-address',
      '127.0.0.1',
      '--provider-config',
      dir,
    ]);
    expect(code).toBe(2);
    expect(stderr).toMatch(/cannot read --provider-config|error reading --provider-config/);
    expect(stderr).not.toContain('is empty');
  });

  it('an EMPTY --provider-config file exits 2 rather than meaning "unconfigured"', async () => {
    // S2: an empty read is as much a failure as an unreadable one. Every provider reads an
    // empty document as "my own defaults", so accepting this would start a gateway that
    // applies none of the operator's intent and prints nothing at all.
    const { code, stderr } = await spawnGatewayExpectingExit([
      '--port',
      String(TEST_PORT + 10),
      '--bind-address',
      '127.0.0.1',
      '--provider-config',
      write('empty.xml', '   \n\t\n'),
    ]);
    expect(code).toBe(2);
    expect(stderr).toContain('is empty');
    // ...and NOT the unreadable wording: this file opened and read perfectly, it simply held
    // nothing, and an operator told "cannot read" would go looking for a permissions problem
    // that is not there. The case above pins the mirror image of this.
    expect(stderr).not.toMatch(/cannot read --provider-config|error reading --provider-config/);
  });

  it("a document the provider rejects exits 2, in the provider's own wording", async () => {
    const { code, stderr } = await spawnGatewayExpectingExit([
      '--port',
      String(TEST_PORT + 10),
      '--bind-address',
      '127.0.0.1',
      '--provider',
      'fastdds',
      '--domain-id',
      '153',
      '--provider-config',
      write('bad-property.xml', ANCHOR_ONLY.replace('PROPERTY_NAME', 'fletcher.max_schema_byte')),
    ]);
    expect(code).toBe(2);
    // The Fast DDS provider's wording, not the gateway's: proof the bytes crossed the seam.
    expect(stderr).toContain('fletcher.max_schema_byte');
  });

  it('a valid --provider-config document reaches the provider and the gateway starts', async () => {
    const child = await spawnGateway(
      { name: 'fastdds', port: TEST_PORT + 9, domainId: '154', roundtripMs: 15_000 },
      [
        '--provider-config',
        write('good.xml', ANCHOR_ONLY.replace('PROPERTY_NAME', 'fletcher.max_schema_bytes')),
      ],
    );
    await stopGateway(child);
  });
});

// Each provider is its own test context, so `npm test` runs every case below
// against both the in-process and FastDDS providers.
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

  // ---------------------------------------------------------------------
  // subscribed text response carries routing only (subId + topic); no
  // schema, no schemaIpc. Locks in the schema-agnostic gateway contract.
  // ---------------------------------------------------------------------
  describe('subscribed response — routing only', () => {
    it('subscribed text frame contains subId + topic and nothing else', async () => {
      const ws = new WebSocket(gatewayUrl);
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
      const ROUNDTRIP_TOPIC = 'protocol/roundtrip';
      const client = new FletcherClient({ url: gatewayUrl });
      await client.connect();

      interface Row {
        sensor_id: number;
        temperature: number;
        label: string;
      }
      const received: Row[] = [];

      // Announce the schema before publishing. The in-process provider ignores
      // it (it loops bytes straight back), but the FastDDS provider buffers
      // subscriber samples until a schema arrives — it never delivers a null
      // schema — so a publish with no prior createTopic would never arrive.
      // Declaring the topic is the correct publisher contract on both.
      await client.createTopic(ROUNDTRIP_TOPIC, HAND_BUILT_SCHEMA);

      const subId = await client.subscribe<Row>(ROUNDTRIP_TOPIC, HAND_BUILT_SCHEMA, (row) => {
        received.push(row);
      });

      const sent: Row[] = [
        { sensor_id: 1, temperature: 23.5, label: 'first' },
        { sensor_id: 42, temperature: -7.125, label: 'second' },
        { sensor_id: 999, temperature: 1.0e9, label: 'third' },
      ];
      for (const row of sent) {
        await client.publish(ROUNDTRIP_TOPIC, HAND_BUILT_SCHEMA, row);
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
      const pub = new FletcherClient({ url: gatewayUrl });
      await pub.connect();
      await pub.createTopic(PUB_TOPIC, HAND_BUILT_SCHEMA);

      // Subscriber: connect raw and inspect the subscribed response
      // directly, so the assertion proves the schema came from the
      // server rather than relying on FletcherClient's internal
      // bookkeeping.
      const ws = new WebSocket(gatewayUrl);
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

      const pub = new FletcherClient({ url: gatewayUrl });
      await pub.connect();
      await pub.createTopic(PUB_TOPIC, HAND_BUILT_SCHEMA);

      const sub = new FletcherClient({ url: gatewayUrl });
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

  // ---------------------------------------------------------------------
  // createTopic is idempotent for an identical schema but rejects a
  // genuine conflict — re-declaring a topic with a different schema.
  // Both providers enforce this, so it runs in both contexts.
  // ---------------------------------------------------------------------
  describe('createTopic schema conflict', () => {
    it('rejects re-declaring a topic with a conflicting schema', async () => {
      const CONFLICT_TOPIC = 'protocol/conflict';
      const client = new FletcherClient({ url: gatewayUrl });
      await client.connect();

      await client.createTopic(CONFLICT_TOPIC, HAND_BUILT_SCHEMA);
      // Same schema again is idempotent — no error.
      await client.createTopic(CONFLICT_TOPIC, HAND_BUILT_SCHEMA);
      // A different schema for the same topic is a genuine conflict.
      await expect(client.createTopic(CONFLICT_TOPIC, MINIMAL_SCHEMA)).rejects.toThrow(
        /conflicting schema/,
      );

      client.close();
    });
  });

  // ---------------------------------------------------------------------
  // Binary frame layouts are exactly what the protocol documents:
  //   server -> client:  [SUB_ID :8 LE][ENVELOPE :rest]
  //   client -> server:  [TOPIC_LEN :2 LE][TOPIC :N][ENVELOPE :rest]
  // ---------------------------------------------------------------------
  describe('binary frame layouts', () => {
    it('server -> client MESSAGE frame is [SUB_ID :8 LE][ENVELOPE]', async () => {
      const FRAME_TOPIC = 'protocol/frame';

      // Announce the schema first so the FastDDS provider will deliver the
      // loopback publish below (it buffers samples until a schema is known); the
      // in-process provider ignores it. Same correct publisher contract on both.
      const announcer = new FletcherClient({ url: gatewayUrl });
      await announcer.connect();
      await announcer.createTopic(FRAME_TOPIC, MINIMAL_SCHEMA);

      const ws = new WebSocket(gatewayUrl);
      ws.binaryType = 'arraybuffer';

      const { subId, rawBytes } = await new Promise<{ subId: bigint; rawBytes: Uint8Array }>(
        (res, rej) => {
          let capturedSubId: bigint | null = null;
          ws.onopen = () => ws.send(buildSubscribe(FRAME_TOPIC));
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
                ws.send(buildPublish(FRAME_TOPIC, env));
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
      announcer.close();
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
});
