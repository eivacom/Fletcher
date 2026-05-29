# @eiva/protoc-gen-fletcher

[![npm (scoped)](https://img.shields.io/npm/v/@eiva/protoc-gen-fletcher?label=latest)](https://www.npmjs.com/package/@eiva/protoc-gen-fletcher)

A `protoc` plugin that generates typed `TypedSchema<T>` modules (`.fletcher.ts`) from `.proto` files. Pairs with [`@eiva/fletcher-gateway-client`](https://www.npmjs.com/package/@eiva/fletcher-gateway-client), Fletcher's WebSocket gateway runtime.

This package is a thin Node.js shim. On first invocation it downloads the platform-matching native plugin binary from [Fletcher's GitHub Releases](https://github.com/eivacom/Fletcher/releases) and caches it under `~/.cache/protoc-gen-fletcher/<version>/`. Subsequent invocations exec the cached binary directly.

## Installation

```bash
npm install --save-dev @eiva/protoc-gen-fletcher
```

`@protobuf-ts/protoc` (which ships the `protoc` compiler binary itself) is declared as a peer dependency. npm 7+ auto-installs it. Older npm versions print a warning naming the missing peer — install it explicitly in that case.

## Usage

Wire `proto:gen` into your `package.json`. Adding it as a `prebuild` script keeps generated bindings in sync with `.proto` changes automatically — every `npm run build` regenerates first.

```json
{
  "scripts": {
    "proto:gen": "protoc --plugin=protoc-gen-fletcher=./node_modules/.bin/protoc-gen-fletcher --fletcher_opt=ts --fletcher_out=src/generated -I proto proto/*.proto",
    "prebuild": "npm run proto:gen",
    "build": "tsc"
  }
}
```

Then:

```bash
npm run build
```

…regenerates `src/generated/*.fletcher.ts` and compiles. Consume the generated descriptor:

```ts
import { FletcherClient } from '@eiva/fletcher-gateway-client';
import { Telemetry, type ITelemetry } from './generated/telemetry.fletcher.js';

const client = new FletcherClient({ url: 'ws://localhost:9090' });
await client.connect();

await client.publish('telemetry', Telemetry, {
  sensor_id: 42,
  temperature: 23.5,
} satisfies ITelemetry);
```

## Supported platforms

| OS / arch | GitHub Release asset |
|---|---|
| `linux/x64` | `protoc-gen-fletcher-linux-x64` |
| `win32/x64` | `protoc-gen-fletcher-windows-x64.exe` |

Adding macOS / arm64 is tracked in [Fletcher's issue tracker](https://github.com/eivacom/Fletcher/issues).

## Environment variables

| Variable | Purpose |
|---|---|
| `PROTOC_GEN_FLETCHER_RELEASES_URL` | Override the GitHub Releases base URL (e.g. to test against a fork or a mirror). Default: `https://github.com/eivacom/Fletcher/releases/download`. |

## License

LGPL-3.0-or-later © The Fletcher Authors. The shim itself is in this repo at [`protoc/npm/`](https://github.com/eivacom/Fletcher/tree/main/protoc/npm); the native plugin source is in [`protoc/`](https://github.com/eivacom/Fletcher/tree/main/protoc).
