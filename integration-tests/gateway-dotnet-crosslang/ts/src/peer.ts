// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The TypeScript peer of the cross-language lane (BIND-8d, D-BIND-116): gateway-client-ts with
// the generated module of crosslang.proto, a WebSocket client of the same gateway as the C#
// gateway client. The same protocol as the native peers:
//
//   * CsToTs: subscribes at start; prints `RECV <seq> OK` or `RECV <seq> MISMATCH <diffs>` for
//     every row, compared field by field with the fixture row its `seq` names.
//   * TsToCs: declares the topic at start, with the Arrow IPC schema the generated CrossRow
//     carries (D-BIND-130), and publishes the four fixture rows on the stdin line `publish`, then
//     prints `PUBLISHED 4`; on `probe` it publishes one row no receiver compares (seq -1) and
//     prints `PROBED` (D-BIND-129).
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
  CrossLang_TsToCsTopic,
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

// D-BIND-130: the generated CrossRow carries its schema as Arrow IPC, so the topic is declared
// with full fidelity, exactly as a native publisher of CrossRow declares it.
await client.createTopic(CrossLang_TsToCsTopic, CrossRow);

await client.subscribe(CrossLang_CsToTsTopic, CrossRow, (row) => {
  try {
    print(reportRow(row.seq, row.row));
  } catch (e) {
    print(`ERROR comparing row: ${(e as Error).message}`);
  }
});

print('READY');

const lines = createInterface({ input: process.stdin });
for await (const line of lines) {
  if (line === 'stop') break;
  if (line === 'probe') {
    // D-BIND-129: a row no receiver compares (seq -1), sent until one arrives.
    try {
      await client.publish(CrossLang_TsToCsTopic, CrossRow, { seq: -1, row: fixtureRow(3) });
      print('PROBED');
    } catch (e) {
      print(`ERROR probing: ${(e as Error).message}`);
    }
  }
  if (line === 'publish') {
    try {
      for (let seq = 0; seq < ROW_COUNT; ++seq) {
        await client.publish(CrossLang_TsToCsTopic, CrossRow, { seq, row: fixtureRow(seq) });
      }
      print(`PUBLISHED ${ROW_COUNT}`);
    } catch (e) {
      print(`ERROR publishing: ${(e as Error).message}`);
    }
  }
}

client.close();
process.exit(0);
