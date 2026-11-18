// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-T-3 (D-BIND-181, D-BIND-184): the generated TypeScript pair of pair.proto's Feed.Send,
// driven over a stub WebSocket that plays the gateway. It checks what each call sends and
// delivers; `npm run typecheck` compiles the generated pair against gateway-client-ts, and the
// `@ts-expect-error` lines below fail that check if the pair stops typing its rows. The live
// gateway lanes (gateway-end-to-end, gateway-dotnet-crosslang) run the pair end to end.
import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { FletcherClient, ObjectBackend, deserializeEnvelope } from '@eiva/fletcher-gateway-client';
import {
  Feed_SendPublisher,
  Feed_SendSubscriber,
  Feed_SendTopic,
  Reading,
  type IReading,
} from '../generated-ts/pair.fletcher.js';

class MockWebSocket {
  static readonly OPEN = 1;
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
    this.onclose?.();
  }
}

function reply(json: object): void {
  MockWebSocket.last.onmessage!({ data: JSON.stringify(json) });
}

const SUB = 5n;
const row: IReading = { id: 7, value: null, tags: ['a', 'b'] };

// A row for subscription SUB, as the gateway sends it: the id, then the envelope the
// publisher's frame carried after its topic.
function deliverPublished(frame: Uint8Array): void {
  const topicLength = new DataView(frame.buffer, frame.byteOffset).getUint16(0, true);
  const envelope = frame.slice(2 + topicLength);
  const message = new Uint8Array(8 + envelope.byteLength);
  new DataView(message.buffer).setBigUint64(0, SUB, true);
  message.set(envelope, 8);
  MockWebSocket.last.onmessage!({ data: message.buffer });
}

async function connect(): Promise<FletcherClient> {
  const client = new FletcherClient({ url: 'ws://gateway' });
  await client.connect();
  return client;
}

async function publishOne(client: FletcherClient, attachments?: Map<string, Uint8Array>) {
  const creating = Feed_SendPublisher.create(client);
  reply({ type: 'topic_created' });
  const publisher = await creating;
  const publishing = publisher.publish(row, attachments);
  reply({ type: 'published' });
  await publishing;
  return MockWebSocket.last.sent.at(-1) as Uint8Array;
}

beforeEach(() => {
  (globalThis as any).WebSocket = MockWebSocket;
});
afterEach(() => {
  delete (globalThis as any).WebSocket;
});

describe('generated pair (Feed.Send)', () => {
  it('binds the topic and the schema', () => {
    expect(Feed_SendTopic).toBe('integration/pair/Feed/Send');
    expect(Feed_SendPublisher.topic).toBe(Feed_SendTopic);
    expect(Feed_SendSubscriber.topic).toBe(Feed_SendTopic);
    expect(Feed_SendPublisher.schema).toBe(Reading);
    expect(Feed_SendSubscriber.schema).toBe(Reading);
  });

  it('create declares the topic with the full-fidelity schema', async () => {
    const client = await connect();
    const creating = Feed_SendPublisher.create(client);
    reply({ type: 'topic_created' });
    expect(await creating).toBeInstanceOf(Feed_SendPublisher);

    expect(Reading.schemaIpc).toBeTruthy();
    expect(JSON.parse(MockWebSocket.last.sent[0] as string)).toEqual({
      action: 'create_topic',
      topic: Feed_SendTopic,
      schemaIpc: Reading.schemaIpc,
    });
    client.close();
  });

  it('publish sends the row and its attachments on the topic', async () => {
    const client = await connect();
    const frame = await publishOne(client, new Map([['k', new Uint8Array([9])]]));

    const topicLength = new DataView(frame.buffer, frame.byteOffset).getUint16(0, true);
    expect(new TextDecoder().decode(frame.slice(2, 2 + topicLength))).toBe(Feed_SendTopic);
    const envelope = deserializeEnvelope(frame.slice(2 + topicLength));
    expect(new ObjectBackend().decode(Reading, envelope.row)).toEqual(row);
    expect(envelope.attachments.get('k')).toEqual(new Uint8Array([9]));
    client.close();
  });

  it('subscribe delivers each row, typed, and unsubscribe ends it', async () => {
    const client = await connect();
    const frame = await publishOne(client);
    const subscriber = new Feed_SendSubscriber(client);
    const received: IReading[] = [];

    const subscribing = subscriber.subscribe((r) => received.push(r));
    expect(JSON.parse(MockWebSocket.last.sent.at(-1) as string)).toEqual({
      action: 'subscribe',
      topic: Feed_SendTopic,
    });
    reply({ type: 'subscribed', subId: SUB.toString(), topic: Feed_SendTopic });
    expect(await subscribing).toBe(SUB);

    deliverPublished(frame);
    expect(received).toEqual([row]);

    const ending = subscriber.unsubscribe(SUB);
    expect(JSON.parse(MockWebSocket.last.sent.at(-1) as string)).toEqual({
      action: 'unsubscribe',
      subId: SUB.toString(),
    });
    reply({ type: 'unsubscribed' });
    await ending;
    deliverPublished(frame);
    expect(received).toHaveLength(1);
    client.close();
  });

  it('subscribeBatched delivers windows, with the options it is given', async () => {
    const client = await connect();
    const frame = await publishOne(client);
    const subscriber = new Feed_SendSubscriber(client);
    const windows: IReading[][] = [];

    const subscribing = subscriber.subscribeBatched((rows) => windows.push(rows), {
      maxRows: 2,
    });
    reply({ type: 'subscribed', subId: SUB.toString(), topic: Feed_SendTopic });
    await subscribing;

    deliverPublished(frame);
    deliverPublished(frame);
    deliverPublished(frame);
    expect(windows).toEqual([[row, row]]);
    client.close();
    expect(windows).toEqual([[row, row], [row]]);
  });
});

// Never called: `npm run typecheck` fails if any of these compiles, that is, if the pair stops
// holding its rows to the message's interface.
export function rowTypesAreChecked(p: Feed_SendPublisher, s: Feed_SendSubscriber): void {
  // @ts-expect-error a string where the schema has an int32
  void p.publish({ id: 'seven', value: null, tags: [] });
  // @ts-expect-error a field the message does not have
  void p.publish({ id: 7, value: null, tags: [], extra: true });
  // @ts-expect-error the row is IReading, not a number
  void s.subscribe((r: number) => r);
}
