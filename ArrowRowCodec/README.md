# ArrowRowCodec

Core serialization library for the ArrowRowSerializer system. Encodes and decodes individual Arrow rows as compact, self-describing binary buffers (`ArrowRow`). Also defines the abstract `PubSubProvider` interface used by generated pub/sub code.

## Contents

| Header | Purpose |
|---|---|
| `include/row_codec.hpp` | `RowCodec` class, `ArrowRow` type alias, wire format documentation |
| `include/pubsub_provider.hpp` | Abstract pub/sub transport interface |
| `include/arrow_row_codec_capi.h` | Pure-C API for cross-language interop |

## ArrowRow wire format

An `ArrowRow` is a `std::vector<uint8_t>` with the following layout (all values little-endian):

```
[SCHEMA_HASH : 8 bytes]   FNV-1a 64-bit hash of the schema fingerprint
[VERSION     : 1 byte ]   0x01

For each field in schema order:
  [NULL_FLAG : 1 byte]   0x00 = present, 0x01 = null

  If present, field payload:
    bool, int*, uint*, float, double, date32/64, timestamp,
    time32/64, duration, fixed_size_binary, decimal128/256,
    interval_*:
        raw bytes, little-endian, no length prefix

    string, large_string, binary, large_binary,
    string_view, binary_view:
        [LENGTH : 4 bytes uint32] [DATA : LENGTH bytes]

    struct:
        fields encoded in order, each preceded by its own NULL_FLAG

    list / large_list:
        [COUNT : 4 bytes uint32] then (NULL_FLAG + element) × COUNT

    fixed_size_list:
        (NULL_FLAG + element) × known_count  — no count prefix

    map:
        [COUNT : 4 bytes uint32] then (key, NULL_FLAG + value) × COUNT
        key has no null flag (Arrow map keys are always non-null)

    sparse_union / dense_union:
        [TYPE_CODE : 1 byte] then active child payload
```

The schema hash allows a receiver to detect schema mismatches before attempting to decode. It is computed by `FingerprintHash(schema)` over Arrow's structural fingerprint (field names, types, nullability; metadata is excluded).

**DICTIONARY types are not supported.** Dictionary encoding is a columnar optimisation whose dictionary array lives outside any individual row; there is no lossless, self-contained per-row encoding for it. Use non-dictionary types instead (e.g. `utf8` instead of `dictionary<int32, utf8>`).

## RowCodec

`RowCodec` binds a schema at construction and provides encode/decode operations. It is not thread-safe for concurrent writes but multiple readers may share a `const RowCodec&`.

```cpp
#include <row_codec.hpp>
using namespace arrow_row;

auto schema = arrow::schema({
    arrow::field("device_id", arrow::int32()),
    arrow::field("value",     arrow::float64()),
});

RowCodec codec(schema);

// Encode
ArrowRow row = codec.EncodeRow({
    std::make_shared<arrow::Int32Scalar>(42),
    std::make_shared<arrow::DoubleScalar>(3.14),
});

// Decode
auto scalars = codec.DecodeRow(row);
// scalars[0] → Int32Scalar(42)
// scalars[1] → DoubleScalar(3.14)
```

`EncodeRow` throws `std::invalid_argument` if the value count or types do not match the schema. `DecodeRow` throws if the buffer is malformed or the schema hash does not match.

## PubSubProvider

`PubSubProvider` is a pure virtual interface that decouples generated pub/sub classes from any concrete transport protocol. Implement it for DDS, MQTT, Zenoh, or any other message bus.

```cpp
class PubSubProvider {
 public:
    // Called once before any Publish/Subscribe on a topic.
    // Schema is available so providers can propagate type info if needed.
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             std::shared_ptr<arrow::Schema> schema) = 0;

    // Publish one encoded row.
    virtual void Publish(const std::vector<std::string>& topic_segments,
                         const ArrowRow& row) = 0;

    // Subscribe. Callback receives raw ArrowRow bytes; decoding is the
    // responsibility of the generated Subscriber class, not the provider.
    using SubscribeCallback = std::function<void(const ArrowRow&)>;
    virtual void Subscribe(const std::vector<std::string>& topic_segments,
                           SubscribeCallback callback) = 0;

    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};
```

Topic names are passed as a `std::vector<std::string>` of segments so the provider can join them with whatever separator the underlying transport requires (`.` for DDS, `/` for MQTT, etc.). The segments come from the proto package, service name, and method name.

## C API

The C API in `arrow_row_codec_capi.h` wraps `RowCodec` for use from C or any language with a C FFI. Schema and record-batch data are exchanged as Arrow IPC stream bytes.

```c
// Create a codec from a schema-only IPC stream
ArrowRowCodec* codec = arrow_row_codec_new(ipc_bytes, ipc_len, &err);

// Encode: takes an IPC stream with exactly one record batch (one row)
uint8_t* out; size_t out_len;
arrow_row_codec_encode_row(codec, ipc_bytes, ipc_len, &out, &out_len, &err);
arrow_row_free_bytes(out);

// Decode: returns an IPC stream with one record batch
arrow_row_codec_decode_row(codec, row_bytes, row_len, &out, &out_len, &err);
arrow_row_free_bytes(out);

arrow_row_codec_free(codec);
```

All returned strings and byte pointers must be freed with `arrow_row_free_string` and `arrow_row_free_bytes` respectively.

## Building

This library is built as part of the root CMake project. It produces two static libraries:

- `row_codec` — core codec; links `arrow::arrow`
- `arrow_row_codec_capi` — C wrapper around `row_codec`

Downstream targets link `row_codec` (C++) or `arrow_row_codec_capi` (C FFI).
