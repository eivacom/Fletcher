// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The TypeScript peer of the cross-language lane (BIND-8d, D-BIND-116; BIND-9b-4, D-BIND-161):
// gateway-client-ts with the generated module of crosslang.proto, a WebSocket client of the same
// gateway as the C# gateway client. The same protocol as the native peers:
//
//   * CsToTs, NativeToTs: subscribes at start; prints `<topic> RECV <seq> OK` or
//     `<topic> RECV <seq> MISMATCH <diffs>` for every row, compared field by field with the
//     fixture row its `seq` names.
//   * TsToCs, TsToNative: declares the topics at start, with the Arrow IPC schema the generated
//     CrossRow carries (D-BIND-130); on the stdin line `publish <topic>` publishes the four
//     fixture rows on it, then prints `PUBLISHED 4`; on `probe <topic>` it publishes one row no
//     receiver compares (seq -1) and prints `PROBED` (D-BIND-129). Any other topic prints `ERROR`.
//
// Prints READY once subscribed, exits on the stdin line `stop`. A handler or publish failure is
// printed as `ERROR <message>`, so the C# side sees it instead of a timeout.
//
// CLI: --url ws://127.0.0.1:<port>
import { createInterface } from 'node:readline';
import { FletcherClient } from '@eiva/fletcher-gateway-client';
import {
  CrossLang_CsToTsSubscriber,
  CrossLang_NativeToTsSubscriber,
  CrossLang_TsToCsPublisher,
  CrossLang_TsToNativePublisher,
  type ICrossRow,
} from '../generated-ts/crosslang.fletcher.js';
import { ROW_COUNT, fixtureRow } from './fixture.js';
import { reportRow } from './compare.js';

function arg(name: string): string {
  const i = process.argv.indexOf(name);
  if (i < 0 || i + 1 >= process.argv.length) throw new Error(`missing ${name}`);
  return process.argv[i + 1];
}

function print(line: string): void {
  process.stdout.write(line + '\n');
}

// Anything else that escapes, printed as ERROR so the C# side names it rather than waiting for a
// row that never comes.
process.on('uncaughtException', (e) => print(`ERROR ${e.message}`));
process.on('unhandledRejection', (e) => print(`ERROR ${(e as Error)?.message ?? String(e)}`));

// A row the client cannot decode, or a callback that throws, is reported here (D-BIND-183).
const client = new FletcherClient({
  url: arg('--url'),
  onError: (fault) =>
    print(`ERROR ${fault.reason}: ${(fault.error as Error)?.message ?? String(fault.error)}`),
});
await client.connect();

// The generated pairs (D-BIND-181, D-BIND-184). A publisher's create declares its topic with the
// Arrow IPC schema the generated CrossRow carries (D-BIND-130), with full fidelity, exactly as a
// native publisher of CrossRow declares it.
const publishers = new Map<string, { publish(row: ICrossRow): Promise<void> }>([
  ['TsToCs', await CrossLang_TsToCsPublisher.create(client)],
  ['TsToNative', await CrossLang_TsToNativePublisher.create(client)],
]);

for (const [name, subscriber] of [
  ['CsToTs', new CrossLang_CsToTsSubscriber(client)],
  ['NativeToTs', new CrossLang_NativeToTsSubscriber(client)],
] as const) {
  await subscriber.subscribe((row) => {
    try {
      print(`${name} ${reportRow(row.seq, row.row)}`);
    } catch (e) {
      print(`ERROR comparing row on ${name}: ${(e as Error).message}`);
    }
  });
}

print('READY');

const lines = createInterface({ input: process.stdin });
for await (const line of lines) {
  if (line === 'stop') break;
  const [command, name = ''] = line.split(' ', 2);
  if (command !== 'probe' && command !== 'publish') continue;
  const publisher = publishers.get(name);
  if (publisher === undefined) {
    print(`ERROR no publisher for topic '${name}'`);
    continue;
  }
  if (command === 'probe') {
    // D-BIND-129: a row no receiver compares (seq -1), sent until one arrives.
    try {
      await publisher.publish({ seq: -1, row: fixtureRow(3) });
      print('PROBED');
    } catch (e) {
      print(`ERROR probing: ${(e as Error).message}`);
    }
  } else {
    try {
      for (let seq = 0; seq < ROW_COUNT; ++seq) {
        await publisher.publish({ seq, row: fixtureRow(seq) });
      }
      print(`PUBLISHED ${ROW_COUNT}`);
    } catch (e) {
      print(`ERROR publishing: ${(e as Error).message}`);
    }
  }
}

client.close();
process.exit(0);
