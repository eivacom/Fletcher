# @eiva/fletcher-gateway-client

[![npm (scoped)](https://img.shields.io/npm/v/@eiva/fletcher-gateway-client?label=latest)](https://www.npmjs.com/package/@eiva/fletcher-gateway-client)
[![npm (scoped with tag)](https://img.shields.io/npm/v/@eiva/fletcher-gateway-client/alpha?label=alpha)](https://www.npmjs.com/package/@eiva/fletcher-gateway-client)

TypeScript client for the Fletcher WebGateway — WebSocket transport with positional row codec.

## Installation

```bash
# latest stable
npm install @eiva/fletcher-gateway-client

# latest alpha
npm install @eiva/fletcher-gateway-client@alpha
```

## Building from source

The build toolchain (pinned Node.js + npm) lives in the repo's devcontainer at
[`.devcontainer/`](../.devcontainer), so the client builds the same way locally and in CI.
In VS Code, *Reopen in Container*; for a plain Docker shell see
[CONTRIBUTING.md](../CONTRIBUTING.md#development-environment). Inside the container:

```bash
cd gateway-client-ts
npm ci         # clean install from package-lock.json
npm run build  # tsc -> dist/
npm test       # vitest run
```

`ci.gateway-client` runs these exact steps inside the shared devcontainer image.

## Usage

**With generated code** (recommended). For `service Positions { rpc Report(stream Position) returns (google.protobuf.Empty); }`,
`protoc-gen-fletcher` with `--fletcher_opt=ts` writes a publisher and a subscriber that carry the
topic and the schema, typed by the message ([`protoc/README.md`](../protoc/README.md#options)):

```ts
import { FletcherClient } from '@eiva/fletcher-gateway-client';
import { Positions_ReportPublisher, Positions_ReportSubscriber } from './positions.fletcher.js';

const client = new FletcherClient({
  url: 'ws://localhost:9090',
  onError: (fault) => console.warn(fault.reason, fault.error),
});
await client.connect();

// Declares the topic with the message's full schema.
const publisher = await Positions_ReportPublisher.create(client);
await publisher.publish({ x: 1.0, y: 2.0 });

const subscriber = new Positions_ReportSubscriber(client);
const id = await subscriber.subscribe((row, attachments) => console.log(row.x, row.y));
await subscriber.unsubscribe(id);

client.close();
```

**By hand**, with a schema written out field by field:

```ts
import { FletcherClient, WireTypeId } from '@eiva/fletcher-gateway-client';
import type { SchemaDescriptor } from '@eiva/fletcher-gateway-client';

const client = new FletcherClient({ url: 'ws://localhost:9090' });
await client.connect();

const schema: SchemaDescriptor = {
  fields: [
    { name: 'x', fieldNumber: 1, wireType: WireTypeId.FLOAT64, nullable: false },
    { name: 'y', fieldNumber: 2, wireType: WireTypeId.FLOAT64, nullable: false },
  ],
};

// Publish
await client.createTopic('position', schema);
await client.publish('position', schema, { x: 1.0, y: 2.0 });

// Subscribe
await client.subscribe('position', schema, (row, attachments) => {
  console.log(row.x, row.y);
});

// List topics
const topics = await client.listTopics();

client.close();
```

## API

### `FletcherClient`

| Method | Description |
|---|---|
| `connect()` | Open the WebSocket connection |
| `createTopic(topic, schema?)` | Create a topic, optionally announcing its schema |
| `subscribe(topic, schema, callback)` | Subscribe to a topic |
| `subscribeBatched(topic, schema, callback, options?)` | Subscribe, receiving rows in windows (below) |
| `unsubscribe(subId)` | Unsubscribe by subscription ID; a batched subscription delivers its pending window first |
| `publish(topic, schema, data, attachments?)` | Publish a row to a topic |
| `listTopics()` | List all topics on the gateway |
| `close()` | Close the connection; batched subscriptions deliver their pending windows first |
| `undecodableRows` | How many rows did not decode against their subscription's schema and were dropped |

A subscription's callback never breaks the connection. When it throws, or a row sent to it does
not decode, the client calls the `onError` option with the reason (`'HandlerThrew'` or
`'RowUndecodable'`), the subscription ID and the error, and goes on with the next row. Without
`onError` the fault is written to `console.error`.

**Batched delivery.** `subscribeBatched` gathers rows into windows and calls
`callback(rows, attachments, status)` once per window, with row `i`'s attachments at index `i`.
These are the rules of the C# gateway client's `SubscribeBatchedAsync` and of the native
`SubscriberArrow`. A window is delivered:

- when it holds `options.maxRows` rows (default 8000), with `status.reason` `'RowLimit'`;
- when `options.timeoutMs` (default 60000, `Infinity` for never) has passed since the previous
  window, with `'Timeout'`. The deadline starts at a window's first row, so after a quiet spell
  longer than the timeout a row is delivered at once;
- when the subscription ends through `unsubscribe`, `close` or the connection closing, with
  `'Closing'`.

A row that does not decode is reported through `onError`, left out, and counted in the window's
`status.rowsDropped`; a window of only such rows is delivered empty. `timeoutMs` above
`MAX_BATCH_TIMEOUT_MS` (2^31-1 ms, about 24.8 days, the longest `setTimeout` waits) is refused.
Browsers throttle timers in hidden tabs, so there a `'Timeout'` window can come late; it is
never lost.

`publish` throws, and sends nothing, when a field the schema marks not nullable is `null` or
absent while its parent is present. In the generated schemas that is every field except an
`optional` one (proto3 or proto2) and a wrapper type such as `google.protobuf.Int32Value`. The message names the field by its path, such as `s.x`,
`readings.item.x` or `tags.value.x`. The C++ codecs and the C# gateway client refuse the same
row, because a C++ row class has nowhere to put the null. A null list element or map value is
accepted, as the native schema marks those nullable.

### `FletcherClientOptions`

| Option | Type | Default | Description |
|---|---|---|---|
| `url` | `string` | — | WebSocket URL, e.g. `ws://localhost:9090` |
| `backend` | `'object' \| 'arrow'` | `'object'` | Decode backend |
| `reconnectDelay` | `number` | `0` | Reconnect delay in ms (0 to disable) |
| `onError` | `(fault) => void` | `console.error` | Called when a subscription's callback throws or a row does not decode |

## License

LGPL-3.0-or-later © The Fletcher Authors
