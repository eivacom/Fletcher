<!-- Space: Software -->
<!-- Parent: Fletcher -->
<!-- Title: Architecture Overview -->

# Fletcher — Architecture Overview

## 1. Vision and Design Principles

Fletcher bridges two worlds: **Protocol Buffers** for message definition and **Apache Arrow** for in-memory analytics. A `protoc` compiler plugin reads `.proto` files and generates C++ wrapper classes that serialize structured data into compact, positional binary row buffers backed by Arrow schemas. These row buffers are designed for scenarios where data arrives one row at a time — telemetry streams, event buses, sensor networks — but is consumed in columnar batches for analytics, storage, or forwarding.

### Design Principles

**Arrow-native from the start.** Every row is defined by an Arrow schema. The generated code, the wire format, and the pub/sub layer all operate on Arrow types. There are no intermediate representations or format conversions — a row encoded on an edge sensor arrives at a server-side consumer without impedance mismatch.

**Edge-compatible by default.** The core pub/sub path — generated message code, the `PubSubProvider` interface, schema transport, positional I/O, and transport providers — depends only on nanoarrow (~100 KB). Server-side features (batching, view classes, `PositionalCodec`) use the full Apache Arrow C++ library through a separate adapter. This two-tier split means edge binaries carry no heavyweight dependencies.

**Code generation over hand-writing.** Schema definitions live in `.proto` files — the single source of truth. The `protoc-gen-fletcher` plugin generates typed C++ message classes, Arrow view classes, and TypeScript interfaces with schema descriptors. No boilerplate to write, no schema drift between languages.

**Transport-agnostic pub/sub.** The `PubSubProvider` interface decouples encoding from transport. Implementations exist for Fast DDS (desktop/server), XRCE-DDS (MCU/embedded), and WebSocket (browser). Adding a new transport (MQTT, Zenoh, custom TCP) requires implementing one interface — no changes to generated code or the codec.

**Zero-copy where it matters.** Publishers encode directly into the transport's buffer via the `RowEncoder` callback pattern, avoiding intermediate copies. Subscribers receive raw bytes and decode in place. The `PositionalWriter`/`PositionalReader` pair operates without allocation.

**Schema transport, not self-description.** The positional wire format omits field numbers, wire type tags, and schema hashes — fields are written in schema order with a compact null bitfield. The publisher's schema is delivered to subscribers via a companion topic mechanism at the provider level, guaranteeing both sides share the same schema without per-row overhead.

---

## 2. Primary Usage Scenarios

### Scenario 1 — Sensor Telemetry Capture

During a survey or inspection mission, sensors produce measurements that must be captured, serialized, and transported. Fletcher handles the path from sensor to subscriber: the protoc plugin generates typed message classes from the sensor's `.proto` definition, the generated publisher serializes each measurement into a compact positional buffer, and the DDS provider transports it reliably over the vessel network.

Critical requirements: low per-row encoding overhead, reliable delivery, schema agreement between publisher and subscriber.

### Scenario 2 — Edge and Embedded Devices

Resource-constrained devices (MCUs, embedded Linux gateways, ROTVs) need to publish Arrow-typed data without the full Arrow C++ library. Fletcher's nanoarrow-only tier provides generated message classes, positional encoding, and XRCE-DDS transport in under 75 KB of Flash. An XRCE-DDS Agent on a more powerful node bridges the data into the full DDS network.

Critical requirements: minimal binary footprint, no dynamic allocation on the hot path, interoperability with desktop/server subscribers.

### Scenario 3 — Browser Visualization

Live sensor data must reach browser-based clients for real-time visualization. The WebGateway exposes the pub/sub layer over WebSocket with a split text/binary protocol. The TypeScript WebClient library decodes the positional wire format in the browser using a pure-TypeScript codec, delivering typed objects to the visualization layer. Schema information is delivered automatically on subscription.

Critical requirements: low-latency WebSocket delivery, schema auto-discovery, no WASM requirement for basic decoding.

---

## 3. System Architecture

### 3.1 Two-Tier Architecture

The system is organized into two deployment tiers that share the same wire format and pub/sub interface.

The **edge tier** depends only on nanoarrow (~100 KB). It includes the generated message classes (`.fletcher.pb.h`), the `PubSubProvider` interface, the `PositionalWriter`/`PositionalReader` pair, the `Driver` (fan-out, subscription IDs), transport providers (FastDDS, XRCE-DDS), and the WebGateway. An edge binary can publish and subscribe to Arrow-typed data streams without linking the full Arrow C++ library.

The **server tier** adds the full Apache Arrow C++ library. It includes the `PositionalCodec` (Arrow scalar encode/decode), the `PubSubArrow` adapter (Arrow C++ convenience types over the nanoarrow provider), and the generated view classes (`.fletcher.view.pb.h`) for zero-copy access into `RecordBatch` and `Table`.

Both tiers produce byte-identical wire format. A row encoded via `PositionalWriter` on an edge device can be decoded by `PositionalCodec` on a server, and vice versa.

### 3.2 Architectural Layers

The system provides seven composable layers:

1. **Codec** — a header-only `PositionalWriter`/`PositionalReader` pair for direct serialization (nanoarrow only), plus a `PositionalCodec` class for server-side ArrowRow encode/decode (Arrow C++).
2. **Protoc Plugin** — generates typed C++ message classes (`.fletcher.pb.h`, nanoarrow only), optional Arrow C++ view classes (`.fletcher.view.pb.h`), and TypeScript interfaces with schema descriptors.
3. **PubSub Provider** — an abstract transport interface operating on raw bytes and nanoarrow `OwnedSchema`, with schema transport, zero-copy `RowEncoder` publishing, and raw-bytes subscriber callbacks.
4. **PubSubArrow** — a server-side wrapper that adds Arrow C++ convenience (arrow::Schema, ArrowRow encode/decode) on top of the nanoarrow provider.
5. **WebGateway** — a Boost.Beast WebSocket server that exposes the Driver to browser clients over a split text/binary protocol.
6. **WebClient** — a TypeScript client library with a pure-TypeScript positional codec and two interchangeable backends (plain objects or Apache Arrow JS).

Each layer is independent. You can use the codec without pub/sub, the web gateway without DDS, or the full pipeline end-to-end.

### 3.3 Data Flow

The end-to-end data flow for a sensor measurement follows this path:

1. A `.proto` file defines the message schema. The protoc plugin generates typed ArrowRow classes and pub/sub code at build time.
2. The sensor driver populates an ArrowRow instance using typed setters.
3. The generated publisher calls `EncodeTo()`, writing the positional wire format directly into the transport provider's buffer (zero-copy via `RowEncoder`).
4. The transport provider (FastDDS, XRCE-DDS) delivers the encoded bytes to subscribers across the network.
5. On the server side, the `PubSubArrow` adapter decodes received bytes into Arrow scalars via `PositionalCodec`.
6. For browser delivery, the WebGateway forwards encoded bytes over WebSocket. The TypeScript WebClient decodes them using the positional codec.

For schema delivery, the flow is:

1. When a publisher creates a topic, it provides the nanoarrow `OwnedSchema`.
2. The transport provider stores the schema alongside the data topic (e.g., via a companion DDS topic with TRANSIENT_LOCAL durability).
3. When a subscriber joins, the provider returns the schema automatically in the `SubscriptionResult`.
4. Both sides now share the same schema, enabling the compact positional wire format.

### 3.4 Component Diagram

See [Component and Dependency Diagram](component-diagram.md).

### 3.5 Data Flow Diagrams

See [Data Flow Diagrams](data-flow-diagrams.md).

---

## 4. Wire Format

### 4.1 Positional Encoding

Every `EncodedRow` is a compact, positional binary buffer (little-endian throughout). Because the publisher's schema is delivered to subscribers via schema transport, both sides are guaranteed the same schema. This allows the wire format to omit field numbers, wire type tags, and schema hashes — fields are written in schema order with a compact null bitfield.

```
[NULL_BITFIELD : ceil(num_fields / 8) bytes, LSB-first bit order]

For each field in schema order (skip if null bit set):
  [PAYLOAD bytes] — size determined by field type from schema
```

For a typical 10-field row (6 fixed scalars, 2 strings, 1 struct, 1 list), the positional format uses ~14 bytes of overhead compared to ~110 bytes for the previous tagged format — an ~87% reduction in per-row overhead.

### 4.2 Type Mapping

Protocol Buffer types map to Arrow types as follows:

| Proto Type | Arrow Type | C++ Storage |
|---|---|---|
| `bool` | `boolean()` | `bool` |
| `int32`, `sint32`, `sfixed32` | `int32()` | `int32_t` |
| `int64`, `sint64`, `sfixed64` | `int64()` | `int64_t` |
| `uint32`, `fixed32` | `uint32()` | `uint32_t` |
| `uint64`, `fixed64` | `uint64()` | `uint64_t` |
| `float` | `float32()` | `float` |
| `double` | `float64()` | `double` |
| `string` | `utf8()` | `std::string` |
| `bytes` | `binary()` | `std::string` |
| `enum` | `int32()` | `int32_t` |
| Nested message | `struct_({child fields...})` | Nested ArrowRow |
| `repeated T` | `list(T)` | `std::vector<T>` |
| `map<K, V>` | `map(K, V)` | Key-value mapping |
| `google.protobuf.Timestamp` | `timestamp(NANO)` | `int64_t` |
| `google.protobuf.Duration` | `duration(NANO)` | `int64_t` |
| `*Value` wrappers | Corresponding scalar, **nullable** | Scalar type |

Proto3 `optional` fields produce nullable Arrow columns. All other singular fields are non-nullable. Wrapper types (`DoubleValue`, etc.) are always nullable.

See [Wire Format Specification](wire-format-specification.md) for full details.

---

## 5. Schema Metadata and Evolution

### 5.1 Schema-Level Metadata

Every generated Arrow schema carries metadata for provenance and evolution:

| Level | Key | Value |
|---|---|---|
| Schema | `proto_package` | The proto `package` declaration |
| Schema | `proto_message` | The message name |
| Field | `field_number` | The proto field number (`= N` assignment) |

Field numbers are the stable identity in Protocol Buffers — they do not change when a field is renamed. Storing them as Arrow field metadata allows schema evolution tracking without the proto descriptor.

### 5.2 Schema Transport

The positional wire format requires both publisher and subscriber to share the same schema. Fletcher solves this via provider-level schema transport:

1. **Publisher calls `CreateTopic(segments, OwnedSchema)`** — the provider stores the schema alongside the data topic.
2. **Subscriber calls `Subscribe(segments, callback)`** — the provider returns a `SubscriptionResult` containing the publisher's schema as an `OwnedSchema`.
3. Both sides use the same schema for the positional wire format.

The FastDDS and XRCE-DDS providers implement this via companion DDS topics (`topic/__schema`) with TRANSIENT_LOCAL durability and KEEP_LAST(1) QoS. The WebGateway delivers the schema in the `subscribed` JSON response in both JSON descriptor and base64 Arrow IPC forms.

---

## 6. Unsupported Constructs

The following Protocol Buffer constructs are not supported, by design:

| Construct | Reason |
|---|---|
| `oneof` | Arrow union is not supported by Parquet and has limited compute kernel coverage. Use separate `optional` fields instead. |
| Recursive messages | Arrow schemas are finite DAGs. No way to represent unbounded recursion as a static schema. |
| `google.protobuf.Any` | Dynamically typed — no static Arrow mapping possible. |
| `google.protobuf.Struct` | Dynamic key set with recursive `Value` oneof. Suffers from both limitations simultaneously. |
| `DICTIONARY` types | Dictionary encoding is a columnar optimization whose dictionary array lives outside any individual row. |

---

## 7. Quick Start

### 7.1 Define your message

```protobuf
// telemetry.proto
syntax = "proto3";
package myapp;

import "google/protobuf/empty.proto";

message SensorReading {
    int32  sensor_id    = 1;
    double temperature  = 2;
    string location     = 3;
    optional double humidity = 4;  // nullable
}

service SensorFeed {
    rpc Stream(stream SensorReading) returns (google.protobuf.Empty);
}
```

### 7.2 Generate code

The `protoc-gen-fletcher` plugin runs during your CMake build. For each `.proto` file it produces:

| File | Depends on | Contents |
|---|---|---|
| `.fletcher.pb.h` | nanoarrow only | Schema function, typed row class, publisher/subscriber |
| `.fletcher.view.pb.h` | Arrow C++ | Immutable view class with typed getters |
| `.fletcher.ts` | — | TypeScript interface, SchemaDescriptor, topic constant |

### 7.3 Encode and decode

```cpp
#include "telemetry.fletcher.pb.h"

// Build a row with typed setters
myapp::SensorReadingArrowRow row;
row.set_sensor_id(42)
   .set_temperature(23.5)
   .set_location("Room 101")
   .set_humidity(55.3);

// Encode to a compact binary buffer (no Arrow C++ needed)
fletcher::EncodedRow encoded = row.Encode();

// Decode back from raw bytes
myapp::SensorReadingArrowRow restored(encoded);
```

### 7.4 Publish and subscribe

```cpp
#include <fast_dds_pubsub_provider.hpp>

auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>();

// Publisher (creates topic with nanoarrow schema, zero-copy encode)
myapp::SensorFeed_StreamPublisher pub(provider);
pub.Publish(row);

// Subscriber (receives typed message, decoded from raw bytes)
myapp::SensorFeed_StreamSubscriber sub(provider);
sub.Subscribe([](myapp::SensorReadingArrowRow msg, fletcher::Attachments att) {
    std::cout << "Temperature: " << msg.temperature() << "\n";
});
```

---

## 8. Building

### 8.1 Prerequisites

- **CMake** 3.15+
- **Conan 2** package manager
- A C++20 compiler (MSVC 2022, GCC 12+, Clang 15+)

### 8.2 C++ Build

```bash
# Install dependencies via Conan
conan install . --build=missing --output-folder=build

# Configure with CMake (uses the Conan-generated toolchain)
cmake --preset conan-default

# Build
cmake --build build/build --config Release
```

### 8.3 TypeScript WebClient

```bash
cd WebClient
npm install
npm test            # 37 Vitest tests
npm run build:ts    # TypeScript compilation
```

### 8.4 Dependencies

| Package | Version | Purpose |
|---|---|---|
| Apache Arrow | 23.0.1 | Columnar data types and schemas (server-side only) |
| Nanoarrow | 0.8.0 | Lightweight Arrow type system (vendored, edge + server) |
| Protocol Buffers | 3.21.12 | Message definitions and compiler |
| Boost | — | Beast/Asio for WebGateway (header-only) |
| Catch2 | 3.7.1 | C++ test framework |
| Fast DDS | 2.14.3 | DDS pub/sub transport |
| nlohmann/json | 3.11.3 | JSON parsing for WebGateway control protocol |
| Node.js | 18+ | TypeScript build and tests (WebClient only) |

---

## 9. Testing

C++ tests use Catch2 v3 and are discovered via CTest. TypeScript tests use Vitest.

```bash
# Run all C++ tests
ctest --test-dir build/build --config Release

# Run specific test suites
ctest --test-dir build/build -R codec          # 92 codec tests
ctest --test-dir build/build -R integration    # 76 integration tests
ctest --test-dir build/build -R pubsub         # 24 pubsub + 7 pubsub_arrow tests

# Run TypeScript tests
cd WebClient && npm test                       # 37 Vitest tests
```

| Suite | Cases | Covers |
|---|---|---|
| Codec (C++) | 92 | Positional codec (all types, nulls, composites), legacy tagged codec, schema evolution, envelope serialization |
| Integration (C++) | 76 | Generated code roundtrips for all proto constructs, pub/sub with mock provider, view classes, GeoArrow |
| PubSub (C++) | 24 | Topic creation, fan-out, subscription IDs, lifecycle, schema transport, schema IPC roundtrips |
| PubSubArrow (C++) | 7 | Arrow C++ / nanoarrow schema conversion, ArrowRow encode/decode through adapter |
| Plugin (C++) | 36 | Type mapping for all proto field kinds |
| WebClient (TS) | 37 | Envelope roundtrips, WS protocol frames, positional codec roundtrips |

See [Technology Decision Log](technology-decisions.md) for rationale behind each technology choice.
