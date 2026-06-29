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

```ts
import { FletcherClient } from '@eiva/fletcher-gateway-client';
import type { SchemaDescriptor } from '@eiva/fletcher-gateway-client';

const client = new FletcherClient({ url: 'ws://localhost:9090' });
await client.connect();

const schema: SchemaDescriptor = {
  fields: [
    { name: 'x', type: 'float64' },
    { name: 'y', type: 'float64' },
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
| `unsubscribe(subId)` | Unsubscribe by subscription ID |
| `publish(topic, schema, data, attachments?)` | Publish a row to a topic |
| `listTopics()` | List all topics on the gateway |
| `close()` | Close the connection |

### `FletcherClientOptions`

| Option | Type | Default | Description |
|---|---|---|---|
| `url` | `string` | — | WebSocket URL, e.g. `ws://localhost:9090` |
| `backend` | `'object' \| 'arrow'` | `'object'` | Decode backend |
| `reconnectDelay` | `number` | `0` | Reconnect delay in ms (0 to disable) |

## License

LGPL-3.0-or-later © The Fletcher Authors
