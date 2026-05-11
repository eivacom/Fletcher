<!-- Space: Software -->
<!-- Parent: Fletcher -->
<!-- Title: Architecture Overview -->

# Fletcher — Architecture Overview

## 1. Vision and Design Principles

Fletcher bridges two worlds: **Protocol Buffers** for message definition and **Apache Arrow** for in-memory analytics. A `protoc` compiler plugin reads `.proto` files and generates C++ wrapper classes that serialize structured data into compact, positional binary row buffers backed by Arrow schemas. These row buffers are designed for scenarios where data arrives one row at a time — telemetry streams, event buses, sensor networks — but is consumed in columnar batches for analytics, storage, or forwarding.

### Design Principles

**Arrow-native from the start.** Every row is defined by an Arrow schema. The generated code, the wire format, and the pub/sub layer all operate on Arrow types. There are no intermediate representations or format conversions — a row encoded on an edge sensor arrives at a server-side consumer without impedance mismatch.

**Edge-compatible by default.** The core pub/sub path — generated message code, the `PubSub` interface, schema transport, positional I/O, and transport providers — depends only on nanoarrow (~100 KB). Server-side features (batching, view classes, `Codec`) use the full Apache Arrow C++ library through a separate adapter. This two-tier split means edge binaries carry no heavyweight dependencies.

**Code generation over hand-writing.** Schema definitions live in `.proto` files — the single source of truth. The `protoc-gen-fletcher` plugin generates typed C++ message classes, Arrow view classes, and TypeScript interfaces with schema descriptors. No boilerplate to write, no schema drift between languages.

**Transport-agnostic pub/sub.** The `PubSub` interface decouples encoding from transport. Implementations exist for Fast DDS (desktop/server), XRCE-DDS (MCU/embedded), and WebSocket (browser). Adding a new transport (MQTT, Zenoh, custom TCP) requires implementing one interface — no changes to generated code or the codec.

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

Live sensor data must reach browser-based clients for real-time visualization. The gateway exposes the pub/sub layer over WebSocket with a split text/binary protocol. The TypeScript gateway-client-ts library decodes the positional wire format using a pure-TypeScript codec, delivering typed objects to the visualization layer. Schema information is delivered automatically on subscription.

Critical requirements: low-latency WebSocket delivery, schema auto-discovery, no WASM requirement for basic decoding (the original WASM-accelerated path was for the legacy tagged-row format and has been removed).

---

## 3. System Architecture

### 3.1 Two-Tier Architecture

The system is organized into two deployment tiers that share the same wire format and pub/sub interface.

The **edge tier** depends only on nanoarrow (~100 KB). It includes the generated message classes (`.fletcher.pb.h`), the `PubSub` interface, the `PositionalWriter`/`PositionalReader` pair, the `Driver` (fan-out, subscription IDs), transport providers (FastDDS, XRCE-DDS), and the gateway. An edge binary can publish and subscribe to Arrow-typed data streams without linking the full Arrow C++ library.

The **server tier** adds the full Apache Arrow C++ library. It includes the `Codec` (Arrow scalar encode/decode), the `pubsub-arrow` adapter (Arrow C++ convenience types over the nanoarrow provider), and the generated Arrow classes (`.fletcher.arrow.pb.h`) for zero-copy view access into `RecordBatch` and `Table`, and `ToArrowRow()` converters.

Both tiers produce byte-identical wire format. A row encoded via `PositionalWriter` on an edge device can be decoded by `Codec` on a server, and vice versa.

### 3.2 Architectural Layers

The system provides seven composable layers:

1. **core** — a header-only library with `PositionalWriter`/`PositionalReader` for direct serialization, `WriteBuffer`, `Envelope`, and `EncodedRow` type aliases. No dependencies beyond the standard library.
2. **arrow-bridge** — the `Codec` class for server-side ArrowRow encode/decode (Arrow C++), plus `ArrowRowView` helpers and CRS utilities.
3. **protoc** — the `protoc-gen-fletcher` plugin. Generates typed C++ message classes (`.fletcher.pb.h`, nanoarrow only), optional Arrow C++ view classes and `ToArrowRow()` converters (`.fletcher.arrow.pb.h`), and TypeScript interfaces with schema descriptors.
4. **pubsub** — an abstract transport interface operating on raw bytes and nanoarrow `OwnedSchema`, with schema transport, zero-copy `RowEncoder` publishing, and raw-bytes subscriber callbacks.
5. **pubsub-arrow** — a server-side wrapper that adds Arrow C++ convenience (arrow::Schema, ArrowRow encode/decode) on top of the nanoarrow provider.
6. **gateway** — a Boost.Beast WebSocket server that exposes the Driver to browser clients over a split text/binary protocol. (Migrated from the prototype's `WebGateway`.)
7. **gateway-client-ts** — a TypeScript client library (npm package `eiva-fletcher-gateway-client`) with a pure-TypeScript positional codec and a plain-object decoder backend. Apache Arrow JS support is stubbed and not implemented.

Each layer is independent. You can use the codec without pub/sub, the web gateway without DDS, or the full pipeline end-to-end.

### 3.3 Data Flow

The end-to-end data flow for a sensor measurement follows this path:

1. A `.proto` file defines the message schema. The protoc plugin generates typed ArrowRow classes and pub/sub code at build time.
2. The sensor driver populates an ArrowRow instance using typed setters.
3. The generated publisher calls `EncodeTo()`, writing the positional wire format directly into the transport provider's buffer (zero-copy via `RowEncoder`).
4. The transport provider (FastDDS, XRCE-DDS) delivers the encoded bytes to subscribers across the network.
5. On the server side, the `pubsub-arrow` adapter decodes received bytes into Arrow scalars via `Codec`.
6. For browser delivery, the gateway forwards encoded bytes over WebSocket. The TypeScript gateway-client-ts decodes them using the positional codec.

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

The FastDDS and XRCE-DDS providers implement this via companion DDS topics (`topic/__schema`) with TRANSIENT_LOCAL durability and KEEP_LAST(1) QoS. The gateway delivers the schema in the `subscribed` JSON response in both JSON descriptor and base64 Arrow IPC forms.

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
| `.fletcher.arrow.pb.h` | Arrow C++ | Immutable view class with typed getters, `ToArrowRow()` converter |
| `.fletcher.ts` | — | TypeScript interface, SchemaDescriptor, topic constant |

### 7.3 Encode and decode

```cpp
#include "telemetry.fletcher.pb.h"

// Build a row with typed setters
fletcher_gen::myapp::SensorReading row;
row.set_sensor_id(42)
   .set_temperature(23.5)
   .set_location("Room 101")
   .set_humidity(55.3);

// Encode to a compact binary buffer (no Arrow C++ needed)
fletcher::EncodedRow encoded = row.Encode();

// Decode back from raw bytes
fletcher_gen::myapp::SensorReading restored(encoded);
```

### 7.4 Publish and subscribe

```cpp
#include <fast_dds_pubsub_provider.hpp>

auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>();

// Publisher (creates topic with nanoarrow schema, zero-copy encode)
fletcher_gen::myapp::SensorFeed_StreamPublisher pub(provider);
pub.Publish(row);

// Subscriber (receives typed message, decoded from raw bytes)
fletcher_gen::myapp::SensorFeed_StreamSubscriber sub(provider);
sub.Subscribe([](fletcher_gen::myapp::SensorReading msg, fletcher::Attachments att) {
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

The monorepo is a polyrepo of Conan packages — each component is built independently into the local Conan cache, and downstream components resolve their dependencies from there. There is no top-level `conanfile.txt` / root build.

```bash
# Build each component into the local Conan cache (order matters for dependencies):
conan create core/.                    --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create arrow-bridge/.            --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create pubsub/.                  --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create pubsub-arrow/.            --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create protoc/.                  --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create fastdds-pubsub-provider/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
conan create xrcedds-pubsub-provider/. --build=missing -pr:a=Ubuntu22-gcc-12-Release
```

A single consolidated devcontainer at `.devcontainer/` (repo root) covers every Fletcher component — Ubuntu 24.04 + GCC 12 + Conan 2 + Node 24. CI builds and locally-opened "Reopen in Container" sessions both use it, sharing one image cache key in Harbor (`dockerrepo.eiva.com/fletcher/devcontainer:cache`).

### 8.3 TypeScript gateway client

```bash
cd gateway-client-ts
npm install
npm test            # vitest suite
npm run build       # TypeScript compilation (tsc)
npm run typecheck   # tsc --noEmit (faster, no dist/ output)
```

### 8.4 Dependencies

| Package | Version | Purpose |
|---|---|---|
| Apache Arrow | 23.0.1 | Columnar data types and schemas (server-side only) |
| Nanoarrow | 0.8.0 | Lightweight Arrow type system (vendored, edge + server) |
| Protocol Buffers | 3.21.12 | Message definitions and compiler |
| Boost | — | Beast/Asio for the `gateway` (header-only) |
| GoogleTest | 1.17.0 | C++ test framework |
| Fast DDS | 2.14.3 | DDS pub/sub transport |
| nlohmann/json | 3.11.3 | JSON parsing for `gateway` control protocol |
| Node.js | 24+ | TypeScript build and tests (`gateway-client-ts` only) |

---

## 9. Testing

C++ tests run through GTest in each component's `tests/` directory and are discovered via CTest. TypeScript tests use Vitest. Each Conan package builds and runs its own tests on `conan create`. A separate cross-component integration test under `integration-tests/protoc-arrow-bridge/` proves that protoc-generated row classes and arrow-bridge's `Codec` agree on every byte for every wire-format feature.

| Suite | Location | Covers |
|---|---|---|
| arrow-bridge | `arrow-bridge/tests/` | Positional codec (all types, nulls, composites), envelope serialization, ArrowRowView accessors, CRS utilities |
| protoc | `protoc/tests/` | Type mapping for all proto field kinds; plugin-level golden output checks |
| pubsub | `pubsub/tests/` | Topic creation, fan-out, subscription IDs, lifecycle, schema transport, schema IPC roundtrips |
| pubsub-arrow | `pubsub-arrow/tests/` | Arrow C++ / nanoarrow schema conversion, ArrowRow encode/decode through adapter |
| fastdds-pubsub-provider | `fastdds-pubsub-provider/tests/` | DDS topic discovery, schema companion topic, end-to-end publish/subscribe |
| xrcedds-pubsub-provider | `xrcedds-pubsub-provider/tests/` | Same surface as the FastDDS provider, against an XRCE-DDS Agent |
| protoc-arrow-bridge integration | `integration-tests/protoc-arrow-bridge/tests/` | Byte-compat invariants between protoc-generated row classes and `Codec` across every wire-format scenario (scalars, WKT, nested, collections, maps, complex, pubsub wrappers, GeoArrow, schema evolution). Also exercises native (no Arrow C++) and ArrowRowView paths. |
| gateway-client-ts | `gateway-client-ts/test/` | Envelope roundtrips, WebSocket protocol frames, positional codec roundtrips, end-to-end client behaviour against a mock WebSocket. |

See [Technology Decision Log](technology-decisions.md) for rationale behind each technology choice.
