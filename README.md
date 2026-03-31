# ArrowRowSerializer

A C++ library system for serialising structured data as compact, self-describing per-row binary buffers using [Apache Arrow](https://arrow.apache.org/) schemas. Designed for telemetry pipelines, message buses, and analytics ingestion where rows arrive one at a time but are ultimately consumed in columnar batches.

## Subprojects

### Codec

The foundational layer. Defines the `EncodedRow` wire format (a schema-hashed binary buffer encoding one Arrow row) and the `RowCodec` class that encodes and decodes it. An `ArrowRow` is a `std::vector<std::shared_ptr<arrow::Scalar>>` representing a single row as Arrow scalars. Also defines the abstract `PubSubProvider` interface that pub/sub generated code programs against.

Used by every other subproject. See [Codec/README.md](Codec/README.md).

---

### Batcher

Accumulates `EncodedRow` buffers and flushes them as an `arrow::Table` once a configured batch size is reached. Includes a write-ahead log (WAL) abstraction backed by SQLite so rows are durable during accumulation.

Use this when you need to bridge a row-at-a-time source (sensor feed, event stream) with a batch-oriented sink (Parquet writer, columnar database). See [Batcher/README.md](Batcher/README.md).

---

### ProtoPlugin

A `protoc` compiler plugin (`protoc-gen-arrow-row`) that reads `.proto` files and generates C++ header files. Each supported proto message gets an ArrowRow wrapper class with typed setters, `ToScalars()` / `Encode()` methods, and the Arrow schema it was generated from. Service definitions with eligible RPC methods additionally generate typed `Publisher` and `Subscriber` classes backed by `PubSubProvider`.

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

## Dependency graph

```
FastDDSPubSubProvider
    └── Codec

ProtoIntegration  (tests only)
    ├── ProtoPlugin  (build-time, via protoc)
    └── Codec

ProtoPlugin
    └── Codec  (pubsub_provider.hpp, row_codec.hpp)

Batcher
    └── Codec

Codec
    └── Apache Arrow
```

## Building

Dependencies are managed with [Conan 2](https://conan.io/). Run from the repository root:

```
conan install . --build=missing --output-folder=build
cmake --preset conan-default
cmake --build build/build --config Release
```

CMake 3.15+ and a C++20-capable compiler are required.
