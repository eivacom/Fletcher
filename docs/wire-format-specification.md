<!-- Space: Software -->
<!-- Parent: Architecture Overview -->
<!-- Title: Wire Format Specification -->

# Wire Format Specification

## Positional Wire Format

Every `EncodedRow` is a compact, positional binary buffer. All multi-byte values are **little-endian**. The format relies on both publisher and subscriber sharing the same schema via [schema transport](data-flow-diagrams.md#schema-transport-flow), allowing it to omit field numbers, wire type tags, and schema hashes.

### Layout

```
[NULL_BITFIELD : ceil(num_fields / 8) bytes, LSB-first bit order]

For each field in schema order (skip if null bit set):
  [PAYLOAD bytes] — size determined by field type from schema
```

The null bitfield uses LSB-first bit ordering: bit 0 of byte 0 corresponds to field 0, bit 1 of byte 0 to field 1, and so on. A set bit means the field is null (payload is omitted).

### Payload Encoding by Kind

| Kind | Encoding |
|---|---|
| Fixed-size scalars (int, float, bool, ...) | Raw little-endian bytes (size known from schema type) |
| String / Binary | `[LEN:4][DATA:LEN]` |
| Struct | `[NULL_BITFIELD:ceil(n/8)]` + per-field payloads (recursive positional) |
| List / Large List | `[COUNT:4][NULL_BITFIELD:ceil(count/8)]` + per-element payloads |
| Fixed-size List | `[NULL_BITFIELD:ceil(fixed_size/8)]` + per-element payloads |
| Map | `[COUNT:4]` + key payloads + `[NULL_BITFIELD:ceil(count/8)]` + value payloads |
| Union | `[TYPE_CODE:1]` + active child payload |

### Wire Size Comparison

For a typical 10-field row (6 fixed scalars, 2 strings, 1 struct, 1 list):

| Format | Overhead |
|---|---|
| Old tagged format | ~110 bytes (8 hash + 2 count + 10 x 10 per-field headers) |
| Positional format | ~14 bytes (2 null bitfield + string/composite length prefixes) |

The positional format achieves ~87% reduction in per-row overhead compared to the previous self-describing tagged format.

---

## Proto to Arrow Type Mapping

### Scalar Types

| Proto Type | Arrow Type | C++ Storage | Notes |
|---|---|---|---|
| `bool` | `boolean()` | `bool` | |
| `int32`, `sint32`, `sfixed32` | `int32()` | `int32_t` | |
| `int64`, `sint64`, `sfixed64` | `int64()` | `int64_t` | |
| `uint32`, `fixed32` | `uint32()` | `uint32_t` | |
| `uint64`, `fixed64` | `uint64()` | `uint64_t` | |
| `float` | `float32()` | `float` | |
| `double` | `float64()` | `double` | |
| `string` | `utf8()` | `std::string` | Setter takes `std::string_view` |
| `bytes` | `binary()` | `std::string` | Setter takes `std::string_view` |
| `enum` | `int32()` | `int32_t` | Enum value cast to int32 |

**Why enum to int32?** Arrow has no native enum type. Storing the integer value preserves the full information, is Parquet-safe, and does not require the consumer to know the proto descriptor. The field metadata includes the field number so schema evolution can be tracked across renames.

### Composite Types

| Proto Construct | Arrow Type | Notes |
|---|---|---|
| Nested message | `struct_({child fields...})` | Recursive positional encoding |
| `repeated T` | `list(T)` | Always non-nullable at list level |
| `repeated Message` | `list(struct_(...))` | Nested struct inside list |
| `map<K, V>` | `map(K, V)` | Keys follow scalar mapping; values may be scalars or structs |

### Well-Known Types

Google's well-known wrapper types are **flattened** to scalar Arrow fields rather than nested structs:

| Proto WKT | Arrow Type | Conversion |
|---|---|---|
| `google.protobuf.Timestamp` | `timestamp(NANO)` | `seconds * 10^9 + nanos` as int64 |
| `google.protobuf.Duration` | `duration(NANO)` | `seconds * 10^9 + nanos` as int64 |
| `google.protobuf.BoolValue` | `boolean()`, **nullable** | Wrapper presence maps to null |
| `google.protobuf.Int32Value` | `int32()`, **nullable** | |
| `google.protobuf.Int64Value` | `int64()`, **nullable** | |
| `google.protobuf.UInt32Value` | `uint32()`, **nullable** | |
| `google.protobuf.UInt64Value` | `uint64()`, **nullable** | |
| `google.protobuf.FloatValue` | `float32()`, **nullable** | |
| `google.protobuf.DoubleValue` | `float64()`, **nullable** | |
| `google.protobuf.StringValue` | `utf8()`, **nullable** | |
| `google.protobuf.BytesValue` | `binary()`, **nullable** | |

`Timestamp` and `Duration` are collapsed to nanosecond int64 because nanoseconds cover the full proto range, `arrow::timestamp` and `arrow::duration` with `TimeUnit::NANO` round-trip cleanly through Parquet, and keeping them as `struct<seconds: int64, nanos: int32>` would require consumers to reconstruct the time value manually.

---

## Nullability Rules

| Proto Declaration | Arrow Nullability |
|---|---|
| Proto3 `optional` field | **Nullable** |
| Proto3 singular field (no `optional`) | **Non-nullable** |
| Proto2 `optional` field | **Nullable** |
| `*Value` wrapper types | Always **nullable** |
| `repeated` and `map` fields | **Non-nullable** (but may be empty) |

---

## Proto to TypeScript Type Mapping

When `--fletcher_opt=ts` is passed, the plugin generates `.fletcher.ts` files:

| Proto Type | TypeScript Type |
|---|---|
| `bool` | `boolean` |
| `int32`, `uint32`, `float`, `double`, `enum` | `number` |
| `int64`, `uint64`, Timestamp, Duration | `bigint` |
| `string` | `string` |
| `bytes` | `Uint8Array` |
| `repeated T` | `T[]` |
| `map<K, V>` | `Map<K, V>` |
| Nested message | Nested `I{Message}` interface |
| `optional` fields | `T | null` |

---

## Schema Metadata

### Schema-Level Metadata

| Key | Value |
|---|---|
| `proto_package` | The proto `package` declaration (e.g. `"myapp"`) |
| `proto_message` | The message name (e.g. `"SensorReading"`) |

### Field-Level Metadata

| Key | Value |
|---|---|
| `field_number` | The proto field number (the `= N` assignment) |

Field numbers are the stable identity in Protocol Buffers; they do not change when a field is renamed. Storing them as Arrow field metadata allows schema evolution tracking without the proto descriptor: a consumer can match columns across schema versions by `field_number` even when field names or positions have changed.

---

## Envelope Wire Format

The `Envelope` bundles an encoded row with optional binary attachments for transport providers that send everything as a single byte stream:

```
[ROW_LEN    : 4 bytes uint32]
[ROW_DATA   : ROW_LEN bytes]     — the EncodedRow (positional format)
[ATT_COUNT  : 4 bytes uint32]    — number of attachments

For each attachment:
  [KEY_LEN  : 4 bytes uint32]
  [KEY_DATA : KEY_LEN bytes]     — attachment key (UTF-8 string)
  [BLOB_LEN : 4 bytes uint32]
  [BLOB_DATA: BLOB_LEN bytes]    — binary blob data
```

The codec is completely unaware of attachments — they remain a transport-layer concern.

### Key Types

| Type | Definition | Purpose |
|---|---|---|
| `Blob` | `shared_ptr<const vector<uint8_t>>` | Zero-copy binary data |
| `Attachments` | `unordered_map<string, Blob>` | Key/value blob pairs |
| `Envelope` | Row + Attachments bundle | Wire transport unit |

---

## WebSocket Protocol

The `gateway` component uses a split text/binary WebSocket protocol:

### Control Messages (JSON text frames)

| Direction | Action / Type | Payload |
|---|---|---|
| Client to Server | `create_topic` | `{"action":"create_topic","topic":"pkg/svc/method"}` |
| Client to Server | `subscribe` | `{"action":"subscribe","topic":"pkg/svc/method"}` |
| Client to Server | `unsubscribe` | `{"action":"unsubscribe","subId":"123"}` |
| Client to Server | `list_topics` | `{"action":"list_topics"}` |
| Server to Client | `topic_created` | `{"type":"topic_created"}` |
| Server to Client | `subscribed` | `{"type":"subscribed","subId":"123","topic":"...","schema":{...},"schemaIpc":"base64..."}` |
| Server to Client | `topics_list` | `{"type":"topics_list","topics":["..."]}` |
| Server to Client | `error` | `{"type":"error","message":"..."}` |

The `subscribed` response includes the schema in two forms:
- `schema` — a JSON object with field names, wire types, nullability, and composite structure (for easy JS consumption)
- `schemaIpc` — base64-encoded Arrow IPC bytes (for full fidelity)

### Data Messages (binary frames)

| Direction | Frame Layout |
|---|---|
| Client to Server (PUBLISH) | `[TOPIC_LEN:2][TOPIC:N][ENVELOPE:rest]` |
| Server to Client (MESSAGE) | `[SUB_ID:8][ENVELOPE:rest]` |

Subscription IDs are stringified in JSON to avoid JavaScript `Number` precision loss. All multi-byte integers in binary frames are little-endian. Each WebSocket connection gets its own session with independent subscriptions that are automatically cleaned up on disconnect.

---

## Unsupported Types

| Type | Reason |
|---|---|
| `DICTIONARY` | Dictionary encoding is a columnar optimization whose dictionary array lives outside any individual row; there is no lossless, self-contained per-row encoding. |
| `oneof` | Arrow union is not supported by Parquet and has limited compute kernel coverage. |
| Recursive messages | Arrow schemas are finite DAGs — no way to represent unbounded recursion. |
| `google.protobuf.Any` | Dynamically typed — no static Arrow mapping possible. |
| `google.protobuf.Struct` | Dynamic key set with recursive `Value` oneof. |
