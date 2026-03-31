# ArrowRowProtoPlugin

A `protoc` compiler plugin (`protoc-gen-arrow-row`) that reads `.proto` files and generates C++ header files containing ArrowRow wrapper classes. Each supported proto message gets a class with a typed setter API that produces an `EncodedRow` via `Encode()` and can convert to/from an `ArrowRow` (vector of scalars), along with the Arrow schema it was generated from. Service definitions with eligible RPC methods additionally generate `Publisher` and `Subscriber` classes for pub/sub.

## How the plugin works

`protoc` invokes the plugin as a subprocess, feeding a serialised `CodeGeneratorRequest` on stdin and reading a `CodeGeneratorResponse` on stdout. The plugin uses `google::protobuf::compiler::PluginMain` to handle the I/O framing, then calls `ArrowRowGenerator::Generate` once per `.proto` file.

For each file the generator:
1. Orders messages dependency-first (DFS topological sort) so nested types are always defined before the messages that reference them.
2. Maps every field via `type_mapper.cpp` to an Arrow type and a C++ setter.
3. Scans all field mappings to discover references to messages in other `.proto` files.
4. Emits one `.arrow_row.pb.h` header containing all generated classes, with `#include` directives for any cross-file generated headers required.

The output file is placed in the directory passed to `--arrow-row_out`.

## Using the plugin in CMake

```cmake
add_custom_command(
    OUTPUT  "${GENERATED_DIR}/${stem}.arrow_row.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-arrow-row=$<TARGET_FILE:protoc-gen-arrow-row>"
            "--arrow-row_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"   # path to google/protobuf/*.proto
            "${PROTO_DIR}/${stem}.proto"
    DEPENDS "${PROTO_DIR}/${stem}.proto" protoc-gen-arrow-row
)
```

## Proto → Arrow type mapping

### Scalar types

All numeric proto wire types map directly to Arrow numeric scalars. Signed variants (int32, sint32, sfixed32) and unsigned variants (fixed32) map to the same Arrow type because Arrow stores the in-memory representation, not the wire encoding.

| Proto type | Arrow type | C++ storage | Notes |
|---|---|---|---|
| `bool` | `arrow::boolean()` | `bool` | |
| `int32`, `sint32`, `sfixed32` | `arrow::int32()` | `int32_t` | |
| `int64`, `sint64`, `sfixed64` | `arrow::int64()` | `int64_t` | |
| `uint32`, `fixed32` | `arrow::uint32()` | `uint32_t` | |
| `uint64`, `fixed64` | `arrow::uint64()` | `uint64_t` | |
| `float` | `arrow::float32()` | `float` | |
| `double` | `arrow::float64()` | `double` | |
| `string` | `arrow::utf8()` | `std::string` | setter takes `std::string_view` |
| `bytes` | `arrow::binary()` | `std::string` | setter takes `std::string_view` |
| `enum` | `arrow::int32()` | `int32_t` | enum value cast to int32 |

**Why enum → int32?** Arrow has no native enum type. Storing the integer value preserves the full information, is Parquet-safe, and does not require the consumer to know the proto descriptor. The field metadata includes the field number so schema evolution can be tracked across renames.

### Nullability

A field is nullable (the Arrow field has `nullable=true`) when:
- It is declared `optional` in proto3 (using the `optional` keyword).
- It is an `optional` field in proto2 (any singular non-required field).

All other singular fields are non-nullable in the generated schema. `repeated` fields are never nullable — an empty list is the proto3 default.

### Nested messages (struct)

A singular message field that is not a well-known type and not recursive is mapped to `arrow::struct_()`.

**Same-file reference:**
```proto
message Address { string city = 1; string country = 2; }
message Person  { string name = 1; Address address = 2; }
```

Generates:
```
Person schema:
  name    : utf8 not null
  address : struct<city: utf8 not null, country: utf8 not null> not null
```

The nested class (`AddressArrowRow`) is emitted before `PersonArrowRow` in the output file because of topological ordering.

**Cross-file reference:**
```proto
// common/address.proto  (package "common")
message Address { string city = 1; string country = 2; }

// orders/person.proto  (package "orders")
import "common/address.proto";
message Person { string name = 1; common.Address address = 2; }
```

`person.arrow_row.pb.h` will contain:
```cpp
#include "common/address.arrow_row.pb.h"   // emitted automatically
```

The generated setter and storage type use a globally-qualified C++ name (`::common::AddressArrowRow`) so the reference resolves regardless of which namespace the consuming code is in.

A deep-nesting warning (depth ≥ 3) is emitted as a code comment for both same-file and cross-file struct fields. The code still compiles but some Arrow consumers (particularly Parquet writers and certain query engines) do not handle arbitrarily deep struct nesting reliably.

### Repeated fields (list)

A `repeated` scalar becomes `arrow::list(element_type)`. A `repeated` message becomes `arrow::list(arrow::struct_(...))`.

```proto
repeated string tags   = 1;   // → list<utf8>
repeated Address addrs = 2;   // → list<struct<city: utf8, country: utf8>>
```

Repeated fields are always non-nullable at the list level. An absent repeated field is represented as an empty list.

### Map fields

`map<K, V>` is mapped to `arrow::map(key_type, value_type)`:

```proto
map<string, double> metrics = 1;   // → map<utf8, float64>
```

Map keys follow the same scalar type mapping as singular scalars. Map values may be scalars, enums, or message types (producing `map<K, struct<...>>`).

**Why a warning is attached to map fields?** Arrow's `map` type has limited support in compute kernels and query engines compared to scalar columns. Additionally, `map<K, struct<V>>` has a fragile Parquet round-trip in some implementations. If the key set is known at schema time, consider using named struct fields instead.

### Well-known types

A subset of `google/protobuf/*.proto` well-known types (WKTs) are flattened into scalar Arrow types rather than emitted as nested structs. This makes the resulting schema more ergonomic for analytics.

| Well-known type | Arrow type | Conversion |
|---|---|---|
| `google.protobuf.Timestamp` | `arrow::timestamp(NANO)` | `seconds * 1e9 + nanos` as int64 |
| `google.protobuf.Duration` | `arrow::duration(NANO)` | `seconds * 1e9 + nanos` as int64 |
| `google.protobuf.BoolValue` | `arrow::boolean()`, **nullable** | wrapper presence → null |
| `google.protobuf.Int32Value` | `arrow::int32()`, **nullable** | |
| `google.protobuf.Int64Value` | `arrow::int64()`, **nullable** | |
| `google.protobuf.UInt32Value` | `arrow::uint32()`, **nullable** | |
| `google.protobuf.UInt64Value` | `arrow::uint64()`, **nullable** | |
| `google.protobuf.FloatValue` | `arrow::float32()`, **nullable** | |
| `google.protobuf.DoubleValue` | `arrow::float64()`, **nullable** | |
| `google.protobuf.StringValue` | `arrow::utf8()`, **nullable** | |
| `google.protobuf.BytesValue` | `arrow::binary()`, **nullable** | |

`*Value` wrapper types are always nullable in the generated schema because that is their sole purpose in proto: expressing "nullable scalar". An absent wrapper message becomes Arrow null; a present one (even with the default inner value) becomes non-null.

`Timestamp` and `Duration` are collapsed to nanosecond int64 because:
- Nanoseconds cover the full proto range.
- `arrow::timestamp` and `arrow::duration` with `TimeUnit::NANO` round-trip cleanly through Parquet.
- Keeping them as `struct<seconds: int64, nanos: int32>` would require consumers to reconstruct the time value manually.

## Schema-level metadata

Every generated Arrow schema carries two metadata keys:

| Key | Value |
|---|---|
| `proto_package` | The proto `package` declaration (e.g. `"integration"`) |
| `proto_message` | The message name (e.g. `"Order"`) |

Every generated Arrow field carries one metadata key:

| Key | Value |
|---|---|
| `field_number` | The proto field number (the `= N` assignment) |

Field numbers are the stable identity in proto; they do not change when a field is renamed. Storing them as Arrow field metadata allows schema evolution tracking without the proto descriptor: a consumer can match columns across schema versions by `field_number` even when field names or positions have changed.

## What is NOT supported and why

### `oneof` fields

```proto
// NOT SUPPORTED
message Payload {
    oneof value {
        int32 count = 1;
        string label = 2;
    }
}
```

Arrow has a `union` type that could represent `oneof`, but Arrow union is not supported by Parquet and has limited compute kernel coverage. Since the primary consumers of this library are analytics pipelines and message buses, union types would create interoperability problems. Use separate `optional` fields instead — the nullability of each field naturally encodes which one is set.

### Recursive messages

```proto
// NOT SUPPORTED
message TreeNode {
    string value       = 1;
    TreeNode left      = 2;   // self-referential
    TreeNode right     = 3;
}
```

Arrow schemas are finite directed acyclic graphs. There is no way to represent an unbounded recursive type as a static Arrow schema. The plugin detects direct and transitive cycles via DFS and skips the field with a comment explaining the reason.

### `google.protobuf.Any`

```proto
// NOT SUPPORTED
google.protobuf.Any payload = 1;
```

`Any` is dynamically typed — the actual message type is embedded in the value at runtime. A static Arrow schema cannot express "any possible message type." There is no meaningful mapping without knowing the actual type at code-generation time.

### `google.protobuf.Struct`

```proto
// NOT SUPPORTED
google.protobuf.Struct attributes = 1;
```

`Struct` has a dynamic key set (`map<string, Value>` where `Value` is a recursive oneof). It suffers from both the oneof and the recursive-message limitations simultaneously.

### proto2 `group` fields

Groups are a proto2-era feature that has been removed in proto3 and is not used in modern proto schemas. They are not supported.

### Service methods: only eligible RPCs generate pub/sub code

A service RPC method generates a `Publisher`/`Subscriber` pair only if it satisfies all of the following:

- The method is **client-streaming** (`stream InputType`).
- The method is **not** server-streaming (`returns` must not be `stream`).
- The return type is **`google.protobuf.Empty`**.
- The input message type has a successful Arrow mapping in the same file.

The reasoning: a publisher sends a stream of rows and expects no reply — exactly the client-streaming-unary pattern. Server streaming or bidirectional streaming imply a request/response cycle that does not fit the pub/sub model. Requiring `Empty` as the return type enforces the fire-and-forget contract. Methods that do not match are emitted as a comment explaining why they were skipped.

## Generated class structure

For each eligible message `Foo`, the plugin generates `FooArrowRow` with:

```cpp
class FooArrowRow {
 public:
    // The Arrow schema for this message, constructed once (Meyers singleton).
    static std::shared_ptr<arrow::Schema> Schema();

    // Typed setters — return *this for chaining.
    FooArrowRow& set_field_name(int32_t value);
    FooArrowRow& set_field_name(std::optional<int32_t> value);   // optional fields
    // ... one setter per supported field

    // Encode the current field values into an EncodedRow buffer.
    arrow_row::EncodedRow Encode() const;
};
```

For each eligible service RPC `Bar` on service `Svc`, the plugin generates:

```cpp
// Publisher: encodes and publishes rows on CreateTopic / Publish.
class Svc_BarPublisher {
 public:
    Svc_BarPublisher(std::shared_ptr<arrow_row::PubSubProvider> provider);
    void Publish(const FooArrowRow& row);
};

// Subscriber: subscribes and decodes, delivering typed scalars to the callback.
class Svc_BarSubscriber {
 public:
    using Callback = std::function<void(/*one arg per schema field*/)>;
    Svc_BarSubscriber(std::shared_ptr<arrow_row::PubSubProvider> provider,
                      Callback callback);
    ~Svc_BarSubscriber();  // calls Unsubscribe on destruction
};
```

Topic name segments passed to the `PubSubProvider` are derived from: `{proto_package, service_name, method_name}`.
