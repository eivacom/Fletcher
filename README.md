# ArrowRowSerializer

A C++ library system for serialising structured data as compact, self-describing per-row binary buffers using [Apache Arrow](https://arrow.apache.org/) schemas. Designed for telemetry pipelines, message buses, and analytics ingestion where rows arrive one at a time but are ultimately consumed in columnar batches. Includes a WebSocket gateway for browser delivery and a TypeScript client library with WASM-accelerated decoding.

## Subprojects

### PubSub

Standalone pub/sub abstraction layer. Defines the `Envelope` type (encoded row + keyed binary attachments), the abstract `PubSubProvider` interface, and the `Driver` class. The Driver wraps a provider with multi-subscriber fan-out (multiple subscribers per topic via `uint64_t` subscription IDs), automatic provider-level subscription lifecycle, and a topic registry with `ListTopics()`/`HasTopic()`.

Extracted from Codec so the pub/sub layer has no dependency on Arrow or the codec.

---

### Codec

The foundational encoding layer. Defines the tagged row wire format (schema-hashed, field-numbered binary buffer encoding one Arrow row) and the `RowCodec` class that encodes and decodes it. Supports schema evolution with field-number-based matching and Iceberg-compliant type promotions. Also provides `RowBatchDecoder` for bulk decoding into `arrow::Table`.

See [Codec/README.md](Codec/README.md).

---

### Batcher

Accumulates `EncodedRow` buffers and flushes them as an `arrow::Table` once a configured batch size is reached. Includes a write-ahead log (WAL) abstraction backed by SQLite so rows are durable during accumulation.

Use this when you need to bridge a row-at-a-time source (sensor feed, event stream) with a batch-oriented sink (Parquet writer, columnar database). See [Batcher/README.md](Batcher/README.md).

---

### ProtoPlugin

A `protoc` compiler plugin (`protoc-gen-arrow-row`) that reads `.proto` files and generates C++ header files. Each supported proto message gets an ArrowRow wrapper class with typed setters, `ToScalars()` / `Encode()` methods, and the Arrow schema it was generated from. Service definitions with eligible RPC methods additionally generate typed `Publisher` and `Subscriber` classes backed by `PubSubProvider`.

With `--arrow-row_opt=ts`, the plugin also emits `.fletcher.ts` files containing TypeScript interfaces, `SchemaDescriptor` constants (with field numbers, wire types, nullability, and nested descriptors), and topic path constants for service methods.

Use this to avoid writing schema and serialisation boilerplate by hand when your data model is already described in Protocol Buffers. See [ProtoPlugin/README.md](ProtoPlugin/README.md).

---

### ProtoIntegration

End-to-end integration tests for the proto plugin. Compiles a set of `.proto` files covering all supported constructs (scalars, optional fields, nested messages, repeated fields, maps, well-known types, and service pub/sub), generates code from them at build time, and runs Catch2 tests to verify the full pipeline.

Not a library — exists only to validate the plugin and codec together. See [ProtoIntegration/README.md](ProtoIntegration/README.md).

---

### FastDDSPubSubProvider

A concrete `PubSubProvider` implementation backed by [eProsima Fast DDS](https://fast-dds.docs.eprosima.com/). Transports `EncodedRow` buffers over a DDS domain using RELIABLE reliability, KEEP_ALL history, and TRANSIENT_LOCAL durability to minimise message loss.

Use this as the transport layer when plugging generated `Publisher`/`Subscriber` classes into a DDS-based system. See [FastDDSPubSubProvider/README.md](FastDDSPubSubProvider/README.md).

---

### WebGateway

A Boost.Beast WebSocket server that exposes the `Driver` to browser clients over an all-binary protocol. Each WebSocket connection gets its own session with independent subscriptions that are automatically cleaned up on disconnect. The binary protocol uses 1-byte action/response tags with little-endian payloads — no JSON involved.

**Binary protocol summary:**

| Direction | Tag | Frame |
|---|---|---|
| Client → Server | `CREATE_TOPIC` (0x01) | `[TAG:1] [TOPIC_LEN:2] [TOPIC:N]` |
| Client → Server | `SUBSCRIBE` (0x02) | `[TAG:1] [TOPIC_LEN:2] [TOPIC:N]` |
| Client → Server | `UNSUBSCRIBE` (0x03) | `[TAG:1] [SUB_ID:8]` |
| Client → Server | `PUBLISH` (0x04) | `[TAG:1] [TOPIC_LEN:2] [TOPIC:N] [ENVELOPE:rest]` |
| Client → Server | `LIST_TOPICS` (0x05) | `[TAG:1]` |
| Server → Client | `SUBSCRIBED` (0x02) | `[TAG:1] [SUB_ID:8] [TOPIC_LEN:2] [TOPIC:N]` |
| Server → Client | `MESSAGE` (0x06) | `[TAG:1] [SUB_ID:8] [ENVELOPE:rest]` |
| Server → Client | `ERROR` (0xFF) | `[TAG:1] [MSG_LEN:2] [MSG:N]` |

Includes a standalone example (`gateway_main.cpp`) with an in-process mock provider.

---

### WebClient

TypeScript client library (`@fletcher/web-client`) for the WebGateway. Connects over WebSocket, decodes/encodes the tagged row wire format, and presents data through two interchangeable backends:

- **Object backend** — zero dependencies, decodes rows into plain `Record<string, unknown>` objects.
- **Arrow backend** — (placeholder) decodes into Apache Arrow JS `RecordBatch` via the optional `apache-arrow` peer dependency.

**Key components:**

- **WASM decoder** (`wasm/src/decoder.c`) — C row parser compiled to WebAssembly via Emscripten. Parses the tagged field table with minimal WASM/JS boundary crossings. Falls back to a pure TypeScript implementation when WASM is unavailable.
- **Row encoder** — pure TypeScript encoder supporting all scalar types (including `bigint` for 64-bit integers), strings, binary, structs, lists, and maps.
- **FletcherClient** — WebSocket manager with `connect()`, `createTopic()`, `subscribe()`, `unsubscribe()`, `publish()`, `listTopics()`, and automatic envelope deserialization with row decoding via the configured backend.
- **Protoc-generated types** — use `--arrow-row_opt=ts` to generate TypeScript interfaces and `SchemaDescriptor` constants from `.proto` files.

```
npm install
npm test         # 35 Vitest tests
npm run build:ts # TypeScript compilation
npm run build:wasm # WASM compilation (requires Emscripten)
```

---

## Dependency graph

```
WebClient (TypeScript)
    └── (connects to WebGateway over WebSocket)

WebGateway
    ├── PubSub (Driver)
    └── Boost.Beast / Boost.Asio

FastDDSPubSubProvider
    └── PubSub

ProtoIntegration  (tests only)
    ├── ProtoPlugin  (build-time, via protoc)
    └── Codec

ProtoPlugin
    └── protobuf::libprotoc

Batcher
    └── Codec

Codec
    ├── PubSub (envelope, pubsub_provider — forwarding headers)
    └── Apache Arrow

PubSub
    └── Apache Arrow (schema parameter only)
```

## Building

Dependencies are managed with [Conan 2](https://conan.io/). Run from the repository root:

```
conan install . --build=missing --output-folder=build
cmake --preset conan-default
cmake --build build/build --config Release
```

CMake 3.15+ and a C++20-capable compiler are required.

For the WebClient TypeScript library:

```
cd WebClient
npm install
npm test
```

To build the WASM decoder (requires [Emscripten](https://emscripten.org/)):

```
cd WebClient
npm run build:wasm
```

To generate TypeScript types from `.proto` files:

```
protoc --plugin=protoc-gen-arrow-row=build/ProtoPlugin/Release/protoc-gen-arrow-row \
       --arrow-row_out=. --arrow-row_opt=ts \
       -I proto -I <protobuf_include> \
       your_messages.proto
```
