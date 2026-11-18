// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-T-2 (D-BIND-183): onError, undecodableRows and subscribeBatched, against a stub
// WebSocket that plays the gateway, with fake timers for the windows' deadlines.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { FletcherClient } from '../src/client.js';
import { MAX_BATCH_TIMEOUT_MS } from '../src/batcher.js';
import type { BatchStatus } from '../src/batcher.js';
import { encodePositional } from '../src/codec/positional-encoder.js';
import { serializeEnvelope } from '../src/envelope.js';
import { WireTypeId } from '../src/wire-types.js';
import type { TypedSchema } from '../src/codec/schema-descriptor.js';
import type { FletcherClientFault } from '../src/types.js';

class MockWebSocket {
  static readonly OPEN = 1;
  static readonly CLOSED = 3;
  static last: MockWebSocket;

  binaryType = 'arraybuffer';
  readyState = MockWebSocket.OPEN;
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;
  sent: (string | Uint8Array)[] = [];

  constructor(public url: string) {
    MockWebSocket.last = this;
    setTimeout(() => this.onopen?.(), 0);
  }

  send(data: string | Uint8Array): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.();
  }
}

interface Row {
  id: number;
}
const schema: TypedSchema<Row> = {
  fields: [{ name: 'id', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false }],
};

const SUB = 7n;

// The gateway's side: a text reply, or a row for the subscription as a binary MESSAGE frame.
function reply(json: object): void {
  MockWebSocket.last.onmessage!({ data: JSON.stringify(json) });
}
function frame(subId: bigint, envelope: Uint8Array): void {
  const buf = new Uint8Array(8 + envelope.byteLength);
  new DataView(buf.buffer).setBigUint64(0, subId, true);
  buf.set(envelope, 8);
  MockWebSocket.last.onmessage!({ data: buf.buffer });
}
function row(id: number, attachments = new Map<string, Uint8Array>()): void {
  frame(SUB, serializeEnvelope({ row: encodePositional(schema, { id }), attachments }));
}
function badRow(): void {
  frame(SUB, new Uint8Array([1, 2, 3]));
}

interface Window {
  ids: number[];
  attachments: Map<string, Uint8Array>[];
  status: BatchStatus;
}

async function connect(onError?: (f: FletcherClientFault) => void): Promise<FletcherClient> {
  const client = new FletcherClient({ url: 'ws://gateway', onError });
  await client.connect();
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance', 'Date'] });
  return client;
}

async function subscribeBatched(
  client: FletcherClient,
  options?: { maxRows?: number; timeoutMs?: number },
  throwOnCall = false,
): Promise<Window[]> {
  const windows: Window[] = [];
  const pending = client.subscribeBatched(
    'topic',
    schema,
    (rows, attachments, status) => {
      windows.push({ ids: rows.map((r) => r.id), attachments, status });
      if (throwOnCall) throw new Error('handler failed');
    },
    options,
  );
  reply({ type: 'subscribed', subId: SUB.toString(), topic: 'topic' });
  expect(await pending).toBe(SUB);
  return windows;
}

beforeEach(() => {
  (globalThis as any).WebSocket = MockWebSocket;
});
afterEach(() => {
  vi.useRealTimers();
  vi.restoreAllMocks();
  delete (globalThis as any).WebSocket;
});

describe('subscribeBatched — when a window flushes', () => {
  it('flushes at maxRows, as RowLimit', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 3 });

    for (let id = 1; id <= 7; ++id) row(id);

    expect(windows.map((w) => w.ids)).toEqual([
      [1, 2, 3],
      [4, 5, 6],
    ]);
    expect(windows.every((w) => w.status.reason === 'RowLimit')).toBe(true);
    client.close();
  });

  it('flushes when the deadline passes, as Timeout, and not before', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100, timeoutMs: 1000 });

    row(1);
    row(2);
    vi.advanceTimersByTime(999);
    expect(windows).toEqual([]);
    vi.advanceTimersByTime(1);

    expect(windows).toEqual([
      {
        ids: [1, 2],
        attachments: [new Map(), new Map()],
        status: { reason: 'Timeout', rowsDropped: 0 },
      },
    ]);
    client.close();
  });

  it('anchors the deadline to the previous flush: a row after a long quiet spell comes at once', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100, timeoutMs: 1000 });

    vi.advanceTimersByTime(5000);
    row(1);

    expect(windows.map((w) => [w.ids, w.status.reason])).toEqual([[[1], 'Timeout']]);
    client.close();
  });

  it('a steady stream flushes once per timeout', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100, timeoutMs: 1000 });

    for (let id = 1; id <= 5; ++id) {
      row(id);
      vi.advanceTimersByTime(400);
    }

    // Rows at 0, 400, 800 | 1200, 1600 | ...: the first window is cut at 1000.
    expect(windows.map((w) => w.ids)).toEqual([
      [1, 2, 3],
      [4, 5],
    ]);
    client.close();
  });

  it('never flushes on time with timeoutMs Infinity', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100, timeoutMs: Infinity });

    row(1);
    vi.advanceTimersByTime(MAX_BATCH_TIMEOUT_MS);

    expect(windows).toEqual([]);
    client.close();
    expect(windows.map((w) => [w.ids, w.status.reason])).toEqual([[[1], 'Closing']]);
  });

  it('treats maxRows below 1 as 1', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 0 });

    row(1);
    row(2);

    expect(windows.map((w) => w.ids)).toEqual([[1], [2]]);
    client.close();
  });
});

describe('subscribeBatched — when the subscription ends', () => {
  it('unsubscribe delivers the pending window as Closing first', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100 });

    row(1);
    const done = client.unsubscribe(SUB);
    expect(windows.map((w) => [w.ids, w.status.reason])).toEqual([[[1], 'Closing']]);
    reply({ type: 'unsubscribed' });
    await done;

    row(2); // after the end: no window
    expect(windows).toHaveLength(1);
    client.close();
  });

  it('close delivers the pending window as Closing', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100 });

    row(1);
    client.close();

    expect(windows.map((w) => [w.ids, w.status.reason])).toEqual([[[1], 'Closing']]);
  });

  it('the connection closing delivers the pending window as Closing', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 100 });

    row(1);
    MockWebSocket.last.onclose!();

    expect(windows.map((w) => [w.ids, w.status.reason])).toEqual([[[1], 'Closing']]);
    vi.advanceTimersByTime(MAX_BATCH_TIMEOUT_MS);
    expect(windows).toHaveLength(1);
  });

  it('delivers nothing on close when the window is empty', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client);

    client.close();

    expect(windows).toEqual([]);
  });
});

describe('subscribeBatched — rows and faults', () => {
  it('keeps row i and its attachments at index i', async () => {
    const client = await connect();
    const windows = await subscribeBatched(client, { maxRows: 2 });

    row(1, new Map([['a', new Uint8Array([1])]]));
    row(2, new Map([['b', new Uint8Array([2])]]));

    expect(windows[0].ids).toEqual([1, 2]);
    expect([...windows[0].attachments[0].keys()]).toEqual(['a']);
    expect([...windows[0].attachments[1].keys()]).toEqual(['b']);
    client.close();
  });

  it('drops an undecodable row, counts it in the window, and reports it', async () => {
    const faults: FletcherClientFault[] = [];
    const client = await connect((f) => faults.push(f));
    const windows = await subscribeBatched(client, { maxRows: 2, timeoutMs: 1000 });

    row(1);
    badRow();
    row(2);

    expect(windows).toEqual([
      {
        ids: [1, 2],
        attachments: [new Map(), new Map()],
        status: { reason: 'RowLimit', rowsDropped: 1 },
      },
    ]);
    expect(faults.map((f) => [f.reason, f.subscriptionId])).toEqual([['RowUndecodable', SUB]]);
    expect(client.undecodableRows).toBe(1);
    client.close();
  });

  it('delivers a window of only dropped rows, empty', async () => {
    const client = await connect(() => {});
    const windows = await subscribeBatched(client, { maxRows: 10, timeoutMs: 1000 });

    badRow();
    badRow();
    vi.advanceTimersByTime(1000);

    expect(windows).toEqual([
      { ids: [], attachments: [], status: { reason: 'Timeout', rowsDropped: 2 } },
    ]);
    client.close();
  });

  it('reports a callback that throws, and goes on delivering', async () => {
    const faults: FletcherClientFault[] = [];
    const client = await connect((f) => faults.push(f));
    const windows = await subscribeBatched(client, { maxRows: 1 }, true);

    row(1);
    row(2);

    expect(windows.map((w) => w.ids)).toEqual([[1], [2]]);
    expect(faults.map((f) => [f.reason, (f.error as Error).message])).toEqual([
      ['HandlerThrew', 'handler failed'],
      ['HandlerThrew', 'handler failed'],
    ]);
    client.close();
  });
});

describe('subscribeBatched — options', () => {
  it.each([
    ['a negative timeout', { timeoutMs: -1 }],
    ['a timeout beyond what a timer can wait', { timeoutMs: MAX_BATCH_TIMEOUT_MS + 1 }],
    ['a NaN timeout', { timeoutMs: NaN }],
    ['a NaN maxRows', { maxRows: NaN }],
  ])('refuses %s before anything is sent', async (_what, options) => {
    const client = await connect();
    const sentBefore = MockWebSocket.last.sent.length;

    await expect(client.subscribeBatched('topic', schema, () => {}, options)).rejects.toThrow(
      RangeError,
    );
    expect(MockWebSocket.last.sent).toHaveLength(sentBefore);
    client.close();
  });
});

describe('subscribe (per row) — faults', () => {
  async function subscribe(client: FletcherClient, onRow: (r: Row) => void): Promise<void> {
    const pending = client.subscribe('topic', schema, (r) => onRow(r));
    reply({ type: 'subscribed', subId: SUB.toString(), topic: 'topic' });
    await pending;
  }

  it('reports an undecodable row and a throwing callback, and delivers the next row', async () => {
    const faults: FletcherClientFault[] = [];
    const client = await connect((f) => faults.push(f));
    const ids: number[] = [];
    await subscribe(client, (r) => {
      ids.push(r.id);
      if (r.id === 1) throw new Error('callback failed');
    });

    row(1);
    badRow();
    row(2);

    expect(ids).toEqual([1, 2]);
    expect(faults.map((f) => f.reason)).toEqual(['HandlerThrew', 'RowUndecodable']);
    expect(client.undecodableRows).toBe(1);
    client.close();
  });

  it('without onError, a fault goes to console.error and the next row is delivered', async () => {
    const error = vi.spyOn(console, 'error').mockImplementation(() => {});
    const client = await connect();
    const ids: number[] = [];
    await subscribe(client, (r) => ids.push(r.id));

    badRow();
    row(2);

    expect(ids).toEqual([2]);
    expect(error).toHaveBeenCalledTimes(1);
    expect(String(error.mock.calls[0][0])).toContain('RowUndecodable');
    client.close();
  });

  it('survives an onError that throws', async () => {
    const client = await connect(() => {
      throw new Error('onError failed');
    });
    const ids: number[] = [];
    await subscribe(client, (r) => ids.push(r.id));

    badRow();
    row(2);

    expect(ids).toEqual([2]);
    client.close();
  });
});

// BIND-T review: D-BIND-185, D-BIND-186, M9 and T12.
describe('review fixes', () => {
  async function subscribe(client: FletcherClient, onRow: (r: Row) => unknown): Promise<void> {
    const pending = client.subscribe('topic', schema, (r) => {
      onRow(r);
    });
    reply({ type: 'subscribed', subId: SUB.toString(), topic: 'topic' });
    await pending;
  }
  const settle = async () => {
    for (let i = 0; i < 5; ++i) await Promise.resolve();
  };

  it('reports an async per-row callback that rejects through onError (D-BIND-185)', async () => {
    const faults: FletcherClientFault[] = [];
    const client = await connect((f) => faults.push(f));
    const pending = client.subscribe('topic', schema, async () => {
      throw new Error('async failed');
    });
    reply({ type: 'subscribed', subId: SUB.toString(), topic: 'topic' });
    await pending;

    row(1);
    await settle();

    expect(faults.map((f) => [f.reason, (f.error as Error).message])).toEqual([
      ['HandlerThrew', 'async failed'],
    ]);
    client.close();
  });

  it('reports an async batch callback that rejects through onError (D-BIND-185)', async () => {
    const faults: FletcherClientFault[] = [];
    const client = await connect((f) => faults.push(f));
    const pending = client.subscribeBatched(
      'topic',
      schema,
      async () => {
        throw new Error('async batch failed');
      },
      { maxRows: 1 },
    );
    reply({ type: 'subscribed', subId: SUB.toString(), topic: 'topic' });
    await pending;

    row(1);
    await settle();

    expect(faults.map((f) => [f.reason, (f.error as Error).message])).toEqual([
      ['HandlerThrew', 'async batch failed'],
    ]);
    client.close();
  });

  it('unsubscribe stops delivery before the gateway replies (D-BIND-186)', async () => {
    const client = await connect();
    const ids: number[] = [];
    await subscribe(client, (r) => ids.push(r.id));

    const ending = client.unsubscribe(SUB);
    row(1); // in flight: sent before the gateway saw the unsubscribe
    reply({ type: 'unsubscribed' });
    await ending;

    expect(ids).toEqual([]);
    client.close();
  });

  it('a refused unsubscribe rejects, and the subscription is still over (D-BIND-186)', async () => {
    const client = await connect();
    const ids: number[] = [];
    await subscribe(client, (r) => ids.push(r.id));

    const ending = client.unsubscribe(SUB);
    reply({ type: 'error', message: 'no such subscription' });
    await expect(ending).rejects.toThrow('no such subscription');

    row(1);
    expect(ids).toEqual([]);
    client.close();
  });

  it("an old socket closing late does not end the new connection's subscriptions (M9)", async () => {
    const client = await connect();
    const old = MockWebSocket.last;
    old.close = function (this: MockWebSocket) {
      this.readyState = MockWebSocket.CLOSED; // its close event comes later
    };
    client.close();

    const reconnecting = client.connect();
    vi.advanceTimersByTime(1);
    await reconnecting;
    const windows = await subscribeBatched(client, { maxRows: 100, timeoutMs: Infinity });
    row(1);

    old.onclose!(); // the old socket's close event, at last
    expect(windows).toEqual([]);

    client.close();
    expect(windows.map((w) => [w.ids, w.status.reason])).toEqual([[[1], 'Closing']]);
  });

  it('publish sends nothing when the encoder refuses the row (T12)', async () => {
    const client = await connect();
    const sentBefore = MockWebSocket.last.sent.length;

    await expect(client.publish('topic', schema, { id: null } as unknown as Row)).rejects.toThrow(
      "field 'id' is not nullable",
    );
    expect(MockWebSocket.last.sent).toHaveLength(sentBefore);
    client.close();
  });
});
