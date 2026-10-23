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
  CrossRow,
  CrossLang_CsToTsTopic,
  CrossLang_NativeToTsTopic,
  CrossLang_TsToCsTopic,
  CrossLang_TsToNativeTopic,
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

// A row the client cannot decode throws inside its message handler, outside any code here:
// reported as ERROR so the C# side names it rather than waiting for a row that never comes.
process.on('uncaughtException', (e) => print(`ERROR ${e.message}`));
process.on('unhandledRejection', (e) => print(`ERROR ${(e as Error)?.message ?? String(e)}`));

const client = new FletcherClient({ url: arg('--url') });
await client.connect();

// D-BIND-130: the generated CrossRow carries its schema as Arrow IPC, so the topics are declared
// with full fidelity, exactly as a native publisher of CrossRow declares them.
const publishTopics = new Map([
  ['TsToCs', CrossLang_TsToCsTopic],
  ['TsToNative', CrossLang_TsToNativeTopic],
]);
for (const topic of publishTopics.values()) {
  await client.createTopic(topic, CrossRow);
}

for (const [name, topic] of [
  ['CsToTs', CrossLang_CsToTsTopic],
  ['NativeToTs', CrossLang_NativeToTsTopic],
] as const) {
  await client.subscribe(topic, CrossRow, (row) => {
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
  const topic = publishTopics.get(name);
  if (topic === undefined) {
    print(`ERROR no publisher for topic '${name}'`);
    continue;
  }
  if (command === 'probe') {
    // D-BIND-129: a row no receiver compares (seq -1), sent until one arrives.
    try {
      await client.publish(topic, CrossRow, { seq: -1, row: fixtureRow(3) });
      print('PROBED');
    } catch (e) {
      print(`ERROR probing: ${(e as Error).message}`);
    }
  } else {
    try {
      for (let seq = 0; seq < ROW_COUNT; ++seq) {
        await client.publish(topic, CrossRow, { seq, row: fixtureRow(seq) });
      }
      print(`PUBLISHED ${ROW_COUNT}`);
    } catch (e) {
      print(`ERROR publishing: ${(e as Error).message}`);
    }
  }
}

client.close();
process.exit(0);
