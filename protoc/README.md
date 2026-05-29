# fletcher-protoc

A `protoc` compiler plugin that reads `.proto` files and generates C++ header files containing row wrapper classes in the `fletcher_gen` namespace, along with TypeScript schema descriptors. Each supported proto message gets a class with a typed setter API that produces an `EncodedRow` via `Encode()`, including the Arrow schema it was generated from. Service definitions with eligible RPC methods additionally generate `Publisher` and `Subscriber` classes for pub/sub.

## Building locally

Requires [Conan 2](https://docs.conan.io/2/) and CMake 3.15+.

### Windows

Build locally:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Build locally and run unit tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

Create the Conan package and run test_package:

```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
```

Create the Conan package with unit tests:

```bash
conan create . -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
```

### Linux (devcontainer)

See the repo root's [Development environment](../README.md#development-environment) section for how to open the devcontainer (VS Code or manual Docker). Once inside, from this directory:

Build locally:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

Build locally and run unit tests:

```bash
conan build . --build=missing -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

Create the Conan package and run test_package:

```bash
conan create . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release
```

Create the Conan package with unit tests:

```bash
conan create . -pr:a=../.conan-profiles/Linux-gcc13-x86_64-Release -o "&:run_tests=True"
```

## Using the plugin

```bash
protoc \
    --plugin=protoc-gen-fletcher=/path/to/fletcher-protoc \
    --fletcher_out=generated/ \
    -I proto/ \
    proto/my_service.proto
```

> **Note:** protoc requires the `--plugin` name to follow the `protoc-gen-<NAME>` convention to map to `--<NAME>_out`. This is a protoc constraint.

### Options

- `--fletcher_opt=ts` — generate TypeScript schema descriptors (`.fletcher.ts`) instead of C++ headers.

## Conan package (C++ consumers)

When consumed as a Conan package, `fletcher-protoc` provides an imported CMake target:

```cmake
find_package(fletcher-protoc REQUIRED)

add_custom_command(
    OUTPUT  "${GENERATED_DIR}/${stem}.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/${stem}.proto"
    DEPENDS "${PROTO_DIR}/${stem}.proto" fletcher-protoc::plugin
)
```

## Using from a TypeScript / npm project

Two npm dev-dependencies wire the plugin into a TypeScript project end-to-end:

- [`@eiva/protoc-gen-fletcher`](https://www.npmjs.com/package/@eiva/protoc-gen-fletcher) — Node shim that downloads the platform-matching native plugin binary from this repo's GitHub Releases on first invocation, caches it under `~/.cache/protoc-gen-fletcher/<version>/`, and exposes it as `protoc-gen-fletcher` in `node_modules/.bin/`.
- [`@protobuf-ts/protoc`](https://www.npmjs.com/package/@protobuf-ts/protoc) — ships the actual `protoc` compiler. Declared as a peer dependency of `@eiva/protoc-gen-fletcher`; npm 7+ auto-installs it.

> **Not** [`@grpc/proto-loader`](https://www.npmjs.com/package/@grpc/proto-loader): that package loads `.proto` files at **runtime** into JavaScript objects via `protobufjs` reflection — a different problem from code generation. It does not produce the typed `TypedSchema<T>` objects that `FletcherClient.publish`/`subscribe` expect.

### 1. Install

```bash
npm install --save-dev @eiva/protoc-gen-fletcher
```

### 2. Wire `proto:gen` as a `prebuild` hook

`package.json`:

```json
{
  "scripts": {
    "proto:gen": "protoc --plugin=protoc-gen-fletcher=./node_modules/.bin/protoc-gen-fletcher --fletcher_opt=ts --fletcher_out=src/generated -I proto proto/*.proto",
    "prebuild": "npm run proto:gen",
    "build": "tsc"
  }
}
```

With `proto:gen` as the `prebuild` hook, every `npm run build` regenerates `.fletcher.ts` files first, then compiles — keeping generated bindings in sync with `.proto` edits automatically. `--fletcher_opt=ts` switches the plugin from its default C++ output to TypeScript schema descriptors.

### Consume the generated descriptors

For a `proto/telemetry.proto` containing:

```proto
syntax = "proto3";
package myapp;

message Telemetry {
  int32 sensor_id = 1;
  double temperature = 2;
  string label = 3;
  bool valid = 4;
  repeated int32 readings = 5;
}
```

`npm run build` writes `src/generated/telemetry.fletcher.ts`. Consume it from application code:

```ts
import { FletcherClient } from '@eiva/fletcher-gateway-client';
import { Telemetry, type ITelemetry } from './generated/telemetry.fletcher.js';

const client = new FletcherClient({ url: 'ws://localhost:9090' });
await client.connect();

await client.createTopic('telemetry', Telemetry);
await client.publish('telemetry', Telemetry, {
  sensor_id: 42,
  temperature: 23.5,
  label: 'intake',
  valid: true,
  readings: [100, 200, 300],
} satisfies ITelemetry);

await client.subscribe('telemetry', Telemetry, (row) => {
  console.log(row.sensor_id, row.temperature);
});
```

The `Telemetry` const is a `TypedSchema<ITelemetry>` — its field layout matches the wire format the C++ side emits, so a publisher and subscriber across the language boundary agree on every byte. The integration test at [`integration-tests/protoc-gateway-client-ts/`](../integration-tests/protoc-gateway-client-ts/) verifies this on every PR.

### Aligning the generated `import` with the published npm name

The generated `.fletcher.ts` files currently emit `import … from 'fletcher-gateway-client'`, but the package on npm is scoped as `@eiva/fletcher-gateway-client`. Bridge the mismatch with a `paths` entry in `tsconfig.json` until the generator is updated:

```jsonc
{
  "compilerOptions": {
    "baseUrl": ".",
    "paths": {
      "fletcher-gateway-client": ["node_modules/@eiva/fletcher-gateway-client"]
    }
  }
}
```

## Proto to Arrow type mapping

| Proto type | Arrow type | Notes |
|---|---|---|
| `bool` | `boolean()` | |
| `int32`, `sint32`, `sfixed32` | `int32()` | |
| `int64`, `sint64`, `sfixed64` | `int64()` | |
| `uint32`, `fixed32` | `uint32()` | |
| `uint64`, `fixed64` | `uint64()` | |
| `float` | `float32()` | |
| `double` | `float64()` | |
| `string` | `utf8()` | |
| `bytes` | `binary()` | |
| `enum` | `int32()` | enum value cast to int32 |
| `google.protobuf.Timestamp` | `timestamp(NANO)` | seconds * 1e9 + nanos |
| `google.protobuf.Duration` | `duration(NANO)` | seconds * 1e9 + nanos |
| `google.protobuf.*Value` | nullable scalar | wrapper presence → null |
| `repeated T` | `list(T)` | |
| `map<K,V>` | `map(K, V)` | |
| nested `message` | `struct<...>` | |
