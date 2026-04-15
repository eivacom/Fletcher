# Foreign Language Bindings — Scope

This document defines which Fletcher libraries, classes, and functions
require foreign language bindings (Rust, C#, Python, etc.) and which are
internal implementation details excluded from the binding surface.

## Projects that need bindings

| Project | Why |
|---------|-----|
| **PubSub** | Core pub/sub abstraction — Driver, PubSub interface, types, positional I/O, OwnedSchema |
| **Codec** | Arrow C++ row codec, positional codec, schema evolution, and the existing C API |
| **PubSubArrow** | Arrow C++ convenience wrapper around Driver |

## Projects that do NOT need bindings

| Project | Why |
|---------|-----|
| **ProtoPlugin** | Executable (`protoc-gen-fletcher`), not a library |
| **FastDDSPubSubProvider** | Provider (driver) implementation — no externally-facing API beyond the `PubSub` interface |
| **XrceDDSPubSubProvider** | Provider (driver) implementation — same reasoning |
| **WebGateway** | Server-side component, not a library |
| **WebClient** | TypeScript client — separate language ecosystem |

---

## Public API surface by library

### PubSub

#### `pubsub/types.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `EncodedRow` | type alias (`std::vector<uint8_t>`) | Binary row payload |
| `Blob` | type alias (`shared_ptr<const vector<uint8_t>>`) | Opaque binary blob |
| `Attachments` | type alias (`unordered_map<string, Blob>`) | Key-value sidecar data |

#### `pubsub/owned_schema.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `OwnedSchema` | class | RAII wrapper for nanoarrow `ArrowSchema` |
| `OwnedSchema::DeepCopy` | static method | Deep-copy from a raw `ArrowSchema*` |
| `SharedSchema` | type alias (`shared_ptr<const ArrowSchema>`) | Shared ownership of a schema |
| `MakeSharedSchema` | free function | Create `SharedSchema` from `OwnedSchema` via aliasing constructor |

#### `pubsub/write_buffer.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `WriteBuffer` | abstract class | Sequential binary output with random-access patching |
| `VectorWriteBuffer` | class | WriteBuffer backed by `std::vector<uint8_t>` |
| `FixedWriteBuffer` | class | WriteBuffer backed by a pre-allocated byte array |

#### `pubsub/positional_io.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `PositionalWriter` | class | Writes positional wire format into a `WriteBuffer` |
| `PositionalWriter::ListContext` | nested struct | Context for writing list elements |
| `PositionalWriter::MapContext` | nested struct | Context for writing map entries |
| `PositionalReader` | class | Reads positional wire format from a byte buffer |
| `PositionalReader::ListHeader` | nested struct | Count + element null bitfield |

#### `pubsub/pubsub.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `PubSub` | abstract class | Provider interface — implement to add a new transport |
| `PubSub::RowEncoder` | type alias (`function<void(WriteBuffer&)>`) | Callback that writes row bytes |
| `PubSub::SubscribeCallback` | type alias | Callback receiving `(data, len, SharedSchema, Attachments)` |
| `SubscriptionResult` | struct | Contains `OwnedSchema schema` |

#### `pubsub/driver.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `Driver` | class | Multi-subscriber fan-out over a single `PubSub` provider |
| `Driver::SubscribeResult` | nested struct | `subscription_id` + `OwnedSchema` |
| `Driver::SubscribeCallback` | type alias | Same signature as `PubSub::SubscribeCallback` |

### Codec

#### `positional_codec.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `ArrowRow` | type alias (`vector<shared_ptr<arrow::Scalar>>`) | One row as Arrow scalars |
| `PositionalCodec` | class | Encode/decode `ArrowRow` ↔ positional wire bytes (Arrow C++ types) |

#### `row_codec.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `RowCodec` | class | Encode/decode `ArrowRow` ↔ tagged wire bytes (Arrow C++ types) |
| `FingerprintHash` | free function | 64-bit FNV-1a hash of a schema's structural fingerprint |

#### `schema_evolution.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `WireTypeId` | enum | Wire-format type identifiers |
| `PromotionKind` | enum | Type promotion classification (identity, widen, illegal, ...) |
| `ArrowTypeToWireTypeId` | free function | Map `arrow::DataType` → `WireTypeId` |
| `ClassifyPromotion` | free function | Determine if a wire type can be promoted to a reader type |
| `DecodingMap` | struct | Maps wire field slots to reader field slots |
| `FieldSlot` | struct | Per-field entry in a `DecodingMap` |
| `BuildDecodingMap` | free function | Build a `DecodingMap` from reader schema + wire header bytes |
| `BuildDecodingMapForStruct` | free function | Build a sub-map for a struct field |

#### `arrow_row_codec_capi.h` (C API)

| Symbol | Kind | Notes |
|--------|------|-------|
| `ArrowRowCodec` | opaque typedef | Handle to a codec instance |
| `arrow_row_codec_new` | function | Create codec from Arrow IPC schema bytes |
| `arrow_row_codec_free` | function | Destroy codec |
| `arrow_row_codec_encode_row` | function | Encode one row (IPC → wire bytes) |
| `arrow_row_codec_decode_row` | function | Decode one row (wire bytes → IPC) |
| `arrow_row_free_string` | function | Free error string |
| `arrow_row_free_bytes` | function | Free output byte buffer |

This C API is the most straightforward binding target — it already has a
stable ABI with opaque types and explicit ownership semantics.

### PubSubArrow

#### `pubsub_arrow/pubsub_arrow.hpp`

| Symbol | Kind | Notes |
|--------|------|-------|
| `PubSubArrow` | class | Arrow C++ convenience wrapper around `Driver` |
| `PubSubArrow::SubscribeResult` | nested struct | `subscription_id` + `shared_ptr<arrow::Schema>` |
| `PubSubArrow::SubscribeCallback` | type alias | Callback receiving `(ArrowRow, Attachments)` |

---

## Internal headers excluded from bindings

These headers are marked "Internal header — not part of the public API" in
their source comments. They are consumed only by providers, the WebGateway,
or generated code and must not be exposed in bindings.

| Header | Used by | Purpose |
|--------|---------|---------|
| `pubsub/envelope.hpp` | Providers, WebGateway | Wire framing for row + attachments |
| `pubsub/schema_ipc.hpp` | Providers, WebGateway | Schema serialization over companion topics |
| `arrow_row_view.hpp` | Generated `.fletcher.arrow.pb.h` | Template infrastructure for view classes |
| `crs_utils.hpp` | Generated code | GeoArrow CRS resolution helpers |
| `Codec/src/row_reader.hpp` | `row_codec.cpp` | Internal byte-level reader |
| `Codec/src/scalar_codec.hpp` | `row_codec.cpp`, `positional_codec.cpp` | Internal scalar encode/decode |

---

## Binding strategy notes

The **C API** (`arrow_row_codec_capi.h`) can be bound directly via FFI in
any language. For the C++ libraries, a C wrapper layer will be needed.

The two tiers of the architecture suggest two tiers of bindings:

- **Edge tier** (nanoarrow only, ~100 KB): `PubSub` library — Driver,
  PositionalReader/Writer, OwnedSchema, WriteBuffer. No Arrow C++ dependency.
  Suitable for embedded or resource-constrained targets.

- **Server tier** (Arrow C++): `Codec` and `PubSubArrow` libraries —
  RowCodec, PositionalCodec, PubSubArrow, schema evolution. Requires Arrow
  C++ at link time.

Bindings for the edge tier should avoid pulling in Arrow C++ as a
dependency, preserving the small-footprint property of the nanoarrow path.
