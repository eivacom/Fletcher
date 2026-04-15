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
| **ProtoPlugin** | Executable (`protoc-gen-fletcher`), not a library - but see section on code generation |
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

## Code generation — `protoc-gen-fletcher`

The protoc plugin does not need runtime bindings (it is a build-time
executable), but it **does** need to emit generated accessor classes for
each supported language — analogous to how `protoc` itself emits C++,
Python, C#, etc.

### What the plugin generates today

For each `.proto` file the plugin produces up to three output files:

| Output file | Tier | Contents |
|-------------|------|----------|
| `.fletcher.pb.h` | Edge (nanoarrow) | Schema builder, mutable row class, Publisher/Subscriber classes |
| `.fletcher.arrow.pb.h` | Server (Arrow C++) | Immutable view class, `ToArrowRow()` free functions |
| `.fletcher.ts` | Web | TypeScript interfaces, schema descriptors, topic constants |

#### `.fletcher.pb.h` — edge-tier row class (C++)

Per message:

| Generated symbol | Purpose |
|------------------|---------|
| `{Msg}Schema()` | Returns `OwnedSchema` with full nanoarrow field metadata |
| `class {Msg}` | Mutable row: constructors (empty, from wire bytes, from `PositionalReader`), typed getters, chainable setters, `Encode()` / `EncodeTo(WriteBuffer&)` |

Per service method (if `service` definitions exist in the `.proto`):

| Generated symbol | Purpose |
|------------------|---------|
| `{Service}_{Method}Publisher` | `TopicSegments()`, `TopicKey()`, `Schema()`, `Publish()` |
| `{Service}_{Method}Subscriber` | Schema discovery, typed subscribe callback |

Dependencies: `nanoarrow`, PubSub library headers only — no Arrow C++.

#### `.fletcher.arrow.pb.h` — server-tier view class (C++)

Per message:

| Generated symbol | Purpose |
|------------------|---------|
| `class {Msg}View` | Immutable view over Arrow scalars; constructors from `ArrowRow`, `StructScalar`, `RecordBatch` row, `Table` row |
| `ToArrowRow({Msg} const&)` | Converts the nanoarrow row class to `fletcher::ArrowRow` (vector of `shared_ptr<arrow::Scalar>`) |

Dependencies: Arrow C++ (`arrow/api.h`, `arrow/c/bridge.h`), `arrow_row_view.hpp`, `positional_codec.hpp`.

#### `.fletcher.ts` — TypeScript web client

Per message:

| Generated symbol | Purpose |
|------------------|---------|
| `interface I{Msg}` | TypeScript interface with typed fields |
| `const {Msg}Schema` | `SchemaDescriptor` for wire decoding |

Per service method:

| Generated symbol | Purpose |
|------------------|---------|
| `const {Service}_{Method}Topic` | Topic path string for PubSub routing |

Dependencies: `@fletcher/web-client` package.

### What each new language needs

To add a language (e.g. Rust, C#, Python), the plugin needs a new code
generator back-end that emits equivalent files.  The generated code in
each language must cover:

1. **Schema builder** — construct the language's Arrow schema
   representation from the proto field metadata (types, nullability,
   field numbers, extension metadata).

2. **Row class** — mutable, with:
   - Typed getters and setters for every field.
   - Encode to / decode from the positional wire format.
   - Support for all mapped types: scalars, nested messages, repeated
     fields, maps, well-known types (Timestamp, Duration, wrapper
     types), and GeoArrow extension types.

3. **Publisher / Subscriber classes** (if the `.proto` has services) —
   typed wrappers around the Driver / PubSub API for the target
   language.

4. **View class** (server tier only) — immutable, zero-copy accessor
   over the language's native Arrow arrays.  Only needed for languages
   that have a full Arrow implementation.

### Type mapping reference

The plugin maps proto types to Arrow and C++ types as follows.  New
language back-ends must provide equivalent mappings for their type
systems.

| Proto type | Arrow type | C++ storage type |
|------------|-----------|------------------|
| `bool` | `boolean` | `bool` |
| `int32`, `sint32`, `sfixed32` | `int32` | `int32_t` |
| `int64`, `sint64`, `sfixed64` | `int64` | `int64_t` |
| `uint32`, `fixed32` | `uint32` | `uint32_t` |
| `uint64`, `fixed64` | `uint64` | `uint64_t` |
| `float` | `float32` | `float` |
| `double` | `float64` | `double` |
| `string` | `utf8` | `std::string` |
| `bytes` | `binary` | `std::string` |
| `enum` | `int32` | `int32_t` |
| `google.protobuf.Timestamp` | `timestamp(ns)` | `int64_t` |
| `google.protobuf.Duration` | `duration(ns)` | `int64_t` |
| `google.protobuf.*Value` | nullable scalar of inner type | `std::optional<T>` |
| `repeated T` (scalar) | `list(T)` | `std::vector<T>` |
| `repeated Message` | `list(struct<…>)` | `std::vector<Msg>` |
| `map<K, V>` | `map(K, V)` | `std::vector<std::pair<K, V>>` |
| Nested message | `struct<…>` | `Msg` (the generated class) |
| GeoArrow types | struct with `ARROW:extension:name` metadata | nested struct |

Unsupported proto features: `oneof`, recursive messages,
`google.protobuf.Any`, `google.protobuf.Struct`, proto2 groups.

---

## Binding architecture

### Two tiers

The architecture has two tiers, and bindings preserve this separation:

- **Edge tier** (nanoarrow only, ~100 KB): `PubSub` library — Driver,
  PositionalReader/Writer, OwnedSchema, WriteBuffer. No Arrow C++ dependency.
  Suitable for embedded or resource-constrained targets.

- **Server tier** (Arrow C++): `Codec` and `PubSubArrow` libraries —
  RowCodec, PositionalCodec, PubSubArrow, schema evolution. Requires Arrow
  C++ at link time.

Bindings for the edge tier must not pull in Arrow C++ as a dependency,
preserving the small-footprint property of the nanoarrow path.

### Shared C API layer

Each C++ library that needs bindings gets a corresponding C API header
with opaque handles and explicit ownership semantics.  All FFI languages
(Rust, C#, Python) bind against the same C API — this avoids maintaining
separate binding layers per language.

| C++ library | C API header | Status |
|-------------|-------------|--------|
| Codec (RowCodec) | `arrow_row_codec_capi.h` | Exists |
| PubSub (Driver) | `pubsub_capi.h` | New |
| PubSubArrow | `pubsub_arrow_capi.h` | Future |

### Native wire-format implementations (not FFI)

PositionalReader, PositionalWriter, and WriteBuffer are **not** exposed
through the C API.  Instead, each target language reimplements them
natively.

**Rationale:** These are called per-field — dozens of times per row
encode/decode.  Per-field FFI round-trips would dominate the cost of
encoding.  A native implementation in each language avoids FFI overhead
entirely, keeps generated code free of `unsafe` / unmanaged calls, and
preserves the edge-tier property (no C++ dependency required for the
generated row classes).

**Consequence:** The positional wire format becomes a cross-language
specification (see below).  If the format evolves, all implementations
must update — the same trade-off `protoc` makes with per-language
protobuf runtimes.

### Performance considerations

**Subscribe (zero-copy):** The subscribe callback receives a pointer to
C++ owned memory `(data, len)`.  The foreign-language callback reads
directly from that pointer for the duration of the callback — no copy
unless the caller needs the data after the callback returns.

**Publish (single memcpy):** The foreign-language side encodes the row
using its native PositionalWriter into a local buffer, then passes the
pre-encoded bytes to the C API.  The C++ side copies those bytes once
into the envelope/provider buffer.  This single `memcpy` of the encoded
row (typically hundreds of bytes) is negligible compared to transport I/O.

**Schema exchange:** Schemas cross the FFI boundary via the
[Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)
(`ArrowSchema` struct), which is already a C-level type.  No
serialization or IPC conversion needed.

### Providers are always C++

PubSub providers (FastDDS, XRCE-DDS, future transports) are always
implemented in C++.  The C API exposes factory functions to create
provider instances; foreign-language consumers interact with providers
only through the Driver handle.

### Code generation is separate from runtime bindings

Runtime bindings (C API + language wrapper) must exist first.  The
`protoc-gen-fletcher` code generator then gets a new back-end per
language that emits accessor classes depending on the runtime crate /
package.

---

## Positional wire format specification

This section defines the binary encoding used by PositionalWriter /
PositionalReader.  All integers are **little-endian**.  Each language
binding must implement this format identically.

### Row / struct layout

```
[NULL_BITFIELD : ceil(num_fields / 8) bytes]
[payload for field 0   (only if field 0 is non-null)]
[payload for field 1   (only if field 1 is non-null)]
...
[payload for field n-1 (only if field n-1 is non-null)]
```

Null bitfield uses LSB-first bit ordering: field `i` corresponds to bit
`i % 8` in byte `i / 8`.  Bit **0** = non-null (payload present), bit
**1** = null (no payload written).  Null fields occupy **zero** bytes in
the payload section.  No alignment or padding between fields.

### Scalar types

| Type | Wire bytes | Encoding |
|------|-----------|----------|
| bool | 1 | `0x00` = false, `0x01` = true |
| int8 / uint8 | 1 | Native byte |
| int16 / uint16 | 2 | LE |
| int32 / uint32 | 4 | LE |
| int64 / uint64 | 8 | LE |
| float32 | 4 | IEEE 754 LE |
| float64 | 8 | IEEE 754 LE |
| timestamp | 8 | int64 LE (units from schema) |
| duration | 8 | int64 LE (units from schema) |
| date32 | 4 | int32 LE (days since epoch) |
| date64 | 8 | int64 LE (ms since epoch) |

### Variable-length types (string, binary)

```
[LENGTH : 4 bytes LE, uint32]
[DATA   : LENGTH bytes]
```

No null terminator.  Strings are UTF-8.

### List

```
[COUNT               : 4 bytes LE, uint32]
[ELEMENT_NULL_BITFIELD : ceil(count / 8) bytes]
[element 0 payload   (if not null)]
[element 1 payload   (if not null)]
...
```

### Fixed-size list

Same as list but **no COUNT prefix** — the fixed size comes from the
schema.

### Map

```
[COUNT              : 4 bytes LE, uint32]
[key 0 payload]
[key 1 payload]
...
[VALUE_NULL_BITFIELD : ceil(count / 8) bytes]
[value 0 payload   (if not null)]
[value 1 payload   (if not null)]
...
```

Keys are never null (no key null bitfield).  All keys are written first,
then the value null bitfield, then non-null value payloads.

### Nested struct

Each struct (including nested) has its own null bitfield.  Structs are
encoded inline at the point where the field appears in the parent.

### Union (sparse / dense)

```
[TYPE_CODE : 1 byte, int8]
[payload for the active variant]
```
