# GIR-3: Language-neutral IR + Edge Encode Vertical Slice

## Summary

GIR-3 introduces the recursive, language-neutral mapped-type IR and migrates only the generated edge C++ `Encode()` / `EncodeTo()` / `EncodeStructTo_()` path to consume that IR as a recursive visitor.

The slice is intentionally narrow:

- Build a new IR in `type_mapper`, replacing unsupported `nullopt` ambiguity with `Unsupported{reason}`.
- Preserve enum identity and symbol information in the IR.
- Represent dictionary as a scalar modifier, not a structural container.
- Move C++ type strings out of the IR into a C++ backend lookup keyed by abstract logical identity.
- Keep `FieldKind` and `FieldMapping` alive as a thin projection over the IR for unmigrated emitters, especially RBA.
- Migrate edge encode only; decode, schema/IPC, view, and TS remain on the adapter until later GIR items.
- Keep GIR-2's byte oracle green: generated `Encode()` must remain byte-identical to `Codec::EncodeRow(ToArrowRow(row))` and committed golden bytes.

The hard invariant is wire byte identity for all currently supported inputs. Generated source will change; encoded bytes must not.

## Design

### IR data model

Add a new language-neutral IR model, preferably in new files:

```text
protoc/include/ir.hpp
protoc/src/ir.cpp
```

The core recursive grammar is:

```cpp
namespace fletcher::ir {

enum class NodeKind {
    SCALAR,
    LIST,
    FIXED_SIZE_LIST,
    STRUCT,
    MAP,
    UNSUPPORTED,
};

enum class LogicalKind {
    BOOL,

    INT8,
    INT16,
    INT32,
    INT64,
    UINT8,
    UINT16,
    UINT32,
    UINT64,

    FLOAT16,
    FLOAT32,
    FLOAT64,

    UTF8,
    BINARY,
    FIXED_SIZE_BINARY,

    DATE32,
    DATE64,
    TIMESTAMP,
    TIME32,
    TIME64,
    DURATION,

    DECIMAL,
    INTERVAL,

    WKT_TIMESTAMP,
    WKT_DURATION,

    // Wrapper WKT nodes use the wrapped scalar kind plus WktKind::WRAPPER.
    // Enum nodes use INT32 physical/logical storage plus EnumIdentity.
};

enum class TimeUnit {
    SECOND,
    MILLI,
    MICRO,
    NANO,
};

enum class IntervalUnit {
    YEAR_MONTH,
    DAY_TIME,
    MONTH_DAY_NANO,
};

enum class WktKind {
    NONE,
    WRAPPER_BOOL,
    WRAPPER_INT32,
    WRAPPER_INT64,
    WRAPPER_UINT32,
    WRAPPER_UINT64,
    WRAPPER_FLOAT,
    WRAPPER_DOUBLE,
    WRAPPER_STRING,
    WRAPPER_BYTES,
    TIMESTAMP,
    DURATION,
};

enum class DictionaryModifier {
    NONE,
    DICTIONARY,
};

struct LogicalType {
    LogicalKind kind;
    std::optional<int32_t> fixed_size_binary_width;
    std::optional<TimeUnit> time_unit;
    std::optional<std::string> timezone;
    std::optional<int32_t> decimal_precision;
    std::optional<int32_t> decimal_scale;
    std::optional<IntervalUnit> interval_unit;
};

struct EnumSymbol {
    std::string name;
    int32_t number;
};

struct EnumIdentity {
    const google::protobuf::EnumDescriptor* descriptor;
    std::string full_name;
    std::vector<EnumSymbol> symbols;
};

struct FieldFacts {
    const google::protobuf::FieldDescriptor* field_descriptor;
    const google::protobuf::Descriptor* containing_message;
    std::string proto_name;
    std::string proto_full_name;
    int32_t wire_field_id;
    bool nullable;
    bool dictionary;
    bool repeated;
    bool map_entry;
    bool proto3_optional;
    bool proto2_optional;
    bool in_real_oneof;
    WktKind wkt;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::string warning;
};

struct StructIdentity {
    const google::protobuf::Descriptor* descriptor;
    std::string full_name;
};

struct IrNode;

struct ScalarNode {
    LogicalType logical_type;
    std::optional<EnumIdentity> enum_identity;
};

struct ListNode {
    std::unique_ptr<IrNode> element;
};

struct FixedSizeListNode {
    std::unique_ptr<IrNode> element;
    int32_t size;
};

struct StructField {
    std::string name;
    int32_t field_number;
    std::unique_ptr<IrNode> type;
    FieldFacts facts;
};

struct StructNode {
    StructIdentity identity;
    std::vector<StructField> fields;
};

struct MapNode {
    std::unique_ptr<IrNode> key;
    std::unique_ptr<IrNode> value;
};

struct UnsupportedNode {
    std::string reason;
};

struct IrNode {
    NodeKind kind;
    FieldFacts facts;

    std::variant<
        ScalarNode,
        ListNode,
        FixedSizeListNode,
        StructNode,
        MapNode,
        UnsupportedNode>
        node;
};

}  // namespace fletcher::ir
```

**Canonical source of optionality and dictionary:** All `nullable` and `dictionary` facts are **canonically stored in `IrNode.facts` only**. Node variants (`ScalarNode`, `ListNode`, `StructNode`, `MapNode`, `FixedSizeListNode`) do not duplicate these fields. This ensures a single source of truth that both the encode visitor and the IR-to-`FieldMapping` projection read from identically.

The model carries:

- Descriptor refs: `FieldDescriptor*`, `Descriptor*`, `EnumDescriptor*`.
- Proto identity: source name, full name, containing message, wire field number.
- Optionality: nullable and dictionary (canonical in `facts`), repeated, proto2/proto3 optional, real oneof.
- WKT markers: wrappers, timestamp, duration.
- Logical identity: abstract `LogicalType`, never backend text.
- Enum identity: descriptor plus symbol table.
- Metadata and warning text.
- Unsupported reason as a node.

There is no C++ type string in this IR. Specifically, the IR must not contain fields like:

```cpp
std::string arrow_type_expr;
std::string storage_type;
std::string param_type;
std::string scalar_ctor;
std::string builder_type;
std::string scalar_type;
std::string default_value;
```

Those belong in a C++ backend lookup table, keyed by `LogicalType`:

```cpp
namespace fletcher::cpp_backend {

struct CppScalarInfo {
    std::string arrow_type_expr;
    std::string storage_type;
    std::string param_type;
    std::string scalar_ctor;
    std::string default_value;
    std::string builder_type;
    std::string scalar_type;
    bool value_is_buffer;
    std::string positional_write;
    std::string positional_read;
};

const CppScalarInfo& LookupScalar(const ir::LogicalType& type,
                                  const std::optional<ir::EnumIdentity>& enum_identity);

}  // namespace fletcher::cpp_backend
```

This is the proof boundary for locked decision #1: IR nodes identify `LogicalKind::INT32`, `LogicalKind::UTF8`, `LogicalKind::WKT_TIMESTAMP`, etc.; only the C++ backend converts those identities to `arrow::int32()`, `int32_t`, `WriteInt32`, and related generated-code strings.

**Arrow physical type and TS WireTypeId are derivable:** Every `LogicalKind` (including `WKT_TIMESTAMP`, `WKT_DURATION`, enum-as-`INT32`, and dictionary-modified scalars) maps to a unique Arrow physical type and a unique TypeScript `WireTypeId` via per-backend lookup tables (C++ and TS), so GIR-5 (schema/IPC) and GIR-7 (TS interface) do not discover a logical fact the IR cannot supply and force a re-design. The IR is complete: all language-neutral classification lives here.

### IR construction in `type_mapper`

Introduce the primary canonical API:

```cpp
namespace fletcher::ir {

IrNode BuildFieldIr(const google::protobuf::FieldDescriptor* field);
StructNode BuildMessageIr(const google::protobuf::Descriptor* message);

}  // namespace fletcher::ir
```

`BuildFieldIr()` dispatches by proto descriptor shape:

1. Real oneof:
   - Return `Unsupported{reason}`.
   - Do not collapse to `nullopt`.
   - Synthetic proto3 optional oneofs remain valid and feed nullable scalar handling.

2. Map field:
   - Return `Map<K,V>`.
   - Key is built as a scalar IR node from the synthetic entry key field.
   - Value is scalar, enum scalar, struct, or unsupported.
   - Map itself is repeated/non-nullable at the proto field level.
   - Dictionary is not involved unless a separate dictionary annotation exists.

3. Repeated scalar or enum:
   - Return `List<Scalar>`.
   - The list is non-nullable; empty list is default.
   - Enum element is `Scalar(LogicalKind::INT32, enum_identity=...)`.

4. Repeated message:
   - If the message is a WKT wrapper, timestamp, or duration, apply WKT handling and then wrap in `List<T>` only if currently supported.
   - If the message has `(fletcher.flatten)`, resolve the flatten chain into nested `List` nodes.
   - Otherwise return `List<Struct>`.

5. Singular message:
   - WKT timestamp maps to scalar logical identity `WKT_TIMESTAMP` with nanosecond unit.
   - WKT duration maps to scalar logical identity `WKT_DURATION` with nanosecond unit.
   - WKT wrappers map to nullable scalar nodes with `WktKind::WRAPPER_*`.
   - `google.protobuf.Any`, `Struct`, dynamic messages, recursive messages, and unsupported WKTs return `Unsupported{reason}`.
   - Flattened wrappers resolve to the inner field's IR, preserving outer nullable facts.

6. Singular enum:
   - Return `Scalar(LogicalKind::INT32, enum_identity=...)`.
   - Preserve `EnumDescriptor*`, full name, and all value names/numbers.

7. Singular primitive:
   - Return `Scalar(logical-kind)` with nullable set in `facts` from `IsFieldNullable(field)`.

Logical mappings:

```text
TYPE_BOOL      -> BOOL
TYPE_INT32     -> INT32
TYPE_SINT32    -> INT32
TYPE_SFIXED32  -> INT32
TYPE_INT64     -> INT64
TYPE_SINT64    -> INT64
TYPE_SFIXED64  -> INT64
TYPE_UINT32    -> UINT32
TYPE_FIXED32   -> UINT32
TYPE_UINT64    -> UINT64
TYPE_FIXED64   -> UINT64
TYPE_FLOAT     -> FLOAT32
TYPE_DOUBLE    -> FLOAT64
TYPE_STRING    -> UTF8
TYPE_BYTES     -> BINARY
TYPE_ENUM      -> INT32 + EnumIdentity
Timestamp      -> WKT_TIMESTAMP / TIMESTAMP(NANO)
Duration       -> WKT_DURATION / DURATION(NANO)
Wrappers       -> wrapped scalar logical kind + nullable + WktKind::WRAPPER_*
```

Flatten resolution moves into IR construction. Chained flatten wrappers produce nested IR nodes directly:

```text
repeated Wrapper(flatten)
  -> List<inner>

repeated Outer(flatten) where Outer contains repeated Inner(flatten)
  -> List<List<leaf>>

singular Wrapper(flatten)
  -> inner with outer nullable propagated
```

Nested scalar-list support becomes representable immediately as `List<List<Scalar>>`; whether all emitters consume it is controlled by the migration schedule. GIR-3's encode visitor can handle it if storage APIs already expose the required nested C++ values; otherwise it may emit `Unsupported{reason}` for unsupported generated storage surfaces while keeping the IR grammar correct.

Dictionary handling follows locked decision #7:

```text
Scalar(logical-kind, enum-identity?, dictionary=true in facts)
```

Dictionary must not become:

```text
Dictionary<K,V>
Struct<dictionary>
List<dictionary>
Map<dictionary>
```

Enum handling follows locked decision #7:

- Enum identity is preserved even while C++ edge storage remains `int32_t`.
- C++ encode writes enum values with the same `WriteInt32` path as today.
- GIR-9 later uses the same `EnumIdentity` to emit typed C++ enum symbols.

Unsupported construction replaces old `nullopt` ambiguity:

```cpp
UnsupportedNode{
    "oneof 'payload' cannot be mapped to a Parquet-safe Arrow type; "
    "consider using separate optional fields instead"
}
```

Examples:

```text
field uses real oneof
field uses google.protobuf.Any
field uses google.protobuf.Struct
field references recursive message
field uses proto2 group
map key type cannot map to scalar
map value type unsupported
flatten wrapper has invalid shape
unknown extension option required for mapping
```

GIR-8 later turns these nodes into clean protoc build errors. GIR-3 only defines and tests the node and keeps existing unsupported behavior at generation boundaries.

### Migration bridge: IR to `FieldMapping` (single canonical source)

`MapField()` is retired as an independent classifier. Instead, it becomes a thin wrapper over the IR:

```cpp
// In type_mapper.hpp/cpp:
// Keep MapField() as a legacy bridge entry point:
std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field) {
    auto ir_node = BuildFieldIr(field);
    return ProjectIrToFieldMapping(ir_node, field->file());
}
```

Introduce the canonical projection:

```cpp
std::optional<FieldMapping> ProjectIrToFieldMapping(
    const ir::IrNode& node,
    const google::protobuf::FileDescriptor* context_file);
```

**Critical invariant:** All unmigrated emitters (RBA, decode, schema, view, TS) consume `FieldMapping` derived from `BuildFieldIr()` via `ProjectIrToFieldMapping()`. There is **no second independent classifier**. This ensures RBA, decode, schema, view, and TS all re-derive from the same IR source and cannot silently drift.

Rules:

- This adapter exists only for unmigrated emitters.
- It is not the source of truth for the edge encode emitter.
- RBA consumes it in this round because RBA is read-only.
- Decode, schema, view, and TS consume it until their GIR items migrate.
- IPC must not permanently consume it; GIR-5 unifies schema and IPC onto the IR schema visitor.
- `FieldKind` is not deleted in GIR-3.

Projection examples:

```text
Scalar               -> FieldKind::SCALAR
List<Scalar>         -> FieldKind::REPEATED_SCALAR
Struct               -> FieldKind::STRUCT
List<Struct>         -> FieldKind::REPEATED_STRUCT
List<List<Struct>>   -> FieldKind::NESTED_LIST with list_depth=2
List<List<List<Struct>>> -> FieldKind::NESTED_LIST with list_depth=3
Map<K,V>             -> FieldKind::MAP
Unsupported          -> nullopt + old UnsupportedReason boundary
```

Projection may fail for IR shapes the old model cannot represent, such as `List<List<Scalar>>`. That is acceptable for unmigrated surfaces. It must be explicit and tested, not silent.

What is migrated now:

```text
GIR-3:
  type_mapper builds IR via BuildFieldIr()
  edge C++ Encode/EncodeTo/EncodeStructTo_ consumes IR recursively
  C++ scalar lookup table introduced
  IR -> FieldMapping adapter via ProjectIrToFieldMapping() feeds all non-encode emitters
  MapField() becomes a thin wrapper over IR + projection
```

What is deferred:

```text
GIR-4:
  edge decode visitor on IR

GIR-5:
  unified schema + IPC visitor on IR

GIR-6:
  Arrow view + ToArrowRow visitor on IR

GIR-7:
  TypeScript interface + descriptor visitor on IR

RIR after BIND-Rust:
  RBA C++/Rust accessor migrates to IR
  FieldKind and FieldMapping retire
```

Adapter retirement plan:

1. GIR-3: IR is canonical; `FieldMapping` projection feeds all non-encode emitters.
2. GIR-4: decode stops using adapter.
3. GIR-5: schema and IPC stop using adapter.
4. GIR-6: view and `ToArrowRow` stop using adapter.
5. GIR-7: TS stops using adapter.
6. RIR: RBA stops using adapter; `FieldKind` and `FieldMapping` are deleted.

### Edge C++ ENCODE emitter as recursive visitor

Replace `EmitFieldEncode(std::ostringstream&, const FieldInfo&, size_t)` with an IR-driven visitor for encode only.

The current `EmitFieldEncode` wire behavior must be preserved exactly:

- Same field order.
- Same positional field index.
- Same null bit behavior.
- Same `BeginStruct(...Schema()->n_children)` behavior.
- Same list count order.
- Same map layout: keys first, then `BeginValues()`, then values.
- Same scalar writer calls.
- Same binary write pointer/size behavior.
- Same default writes for unset non-nullable scalar fields.

Introduce an encode context:

```cpp
namespace fletcher::cpp_backend {

enum class ValueAccessMode {
    RAW_VALUE,           // list element, map key/value, struct leaf
    DEREF_OPTIONAL,      // nullable top-level: *opt
    VALUE_OR_DEFAULT,    // non-nullable top-level: opt.value_or(default)
};

struct EncodeContext {
    std::ostringstream& out;
    std::string writer_expr;      // usually "w"
    std::string value_expr;       // e.g. "field_", "field_[li_]", "v"
    ValueAccessMode access_mode;  // how to dereference value_expr
    std::string field_index_expr; // e.g. "0"; only relevant for top-level null bit
    std::string indent;
    int depth;
    bool top_level_field;
    bool nullable_position;
    const google::protobuf::FileDescriptor* context_file;
};

class EdgeEncodeVisitor {
public:
    explicit EdgeEncodeVisitor(EncodeContext ctx);

    void EmitField(const ir::IrNode& node);
    void EmitValue(const ir::IrNode& node, const EncodeContext& ctx);

private:
    void EmitScalar(const ir::ScalarNode& scalar, const ir::IrNode& node,
                    const EncodeContext& ctx);
    void EmitList(const ir::ListNode& list, const ir::IrNode& node,
                  const EncodeContext& ctx);
    void EmitStruct(const ir::StructNode& st, const ir::IrNode& node,
                    const EncodeContext& ctx);
    void EmitMap(const ir::MapNode& map, const ir::IrNode& node,
                 const EncodeContext& ctx);
    void EmitUnsupported(const ir::UnsupportedNode& unsupported,
                         const EncodeContext& ctx);
};

}  // namespace fletcher::cpp_backend
```

The visitor maps IR recursion to generated code recursion. Top-level field null-handling:

```cpp
void EdgeEncodeVisitor::EmitField(const ir::IrNode& node) {
    const bool nullable = node.facts.nullable;
    if (nullable && ctx.top_level_field) {
        out << indent << "if (!" << value_expr << ".has_value()) "
            << writer_expr << ".SetNull(" << field_index_expr << ");\n"
            << indent << "else {\n";
        auto inner = ctx;
        inner.access_mode = ValueAccessMode::DEREF_OPTIONAL;
        inner.value_expr = value_expr;  // stays as-is; access mode handles dereferencing
        inner.indent += "    ";
        inner.top_level_field = false;
        EmitValue(node, inner);
        out << indent << "}\n";
        return;
    }

    // Non-nullable top-level: use .value_or(default)
    if (ctx.top_level_field && !nullable) {
        auto inner = ctx;
        inner.access_mode = ValueAccessMode::VALUE_OR_DEFAULT;
        inner.value_expr = value_expr;
        EmitValue(node, inner);
        return;
    }

    // Inside a container (list element, map key/value, struct leaf): raw value
    EmitValue(node, ctx);
}
```

Scalar visitor with explicit access mode:

```cpp
void EdgeEncodeVisitor::EmitScalar(const ir::ScalarNode& scalar,
                                   const ir::IrNode& node,
                                   const EncodeContext& ctx) {
    const auto& scalar_info = cpp_backend::LookupScalar(scalar.logical_type,
                                                        scalar.enum_identity);
    
    // Construct the final value expression based on access mode
    std::string final_expr = ctx.value_expr;
    if (ctx.access_mode == ValueAccessMode::DEREF_OPTIONAL) {
        final_expr = "*" + ctx.value_expr;
    } else if (ctx.access_mode == ValueAccessMode::VALUE_OR_DEFAULT) {
        final_expr = ctx.value_expr + ".value_or(" + scalar_info.default_value + ")";
    }
    // else RAW_VALUE: use final_expr as-is

    // Emit the scalar write; handle binary separately for pointer/size
    if (scalar.logical_type.kind == ir::LogicalKind::BINARY) {
        out << ctx.indent << ctx.writer_expr 
            << ".WriteBinary(reinterpret_cast<const uint8_t*>(" 
            << final_expr << ".data()), " << final_expr << ".size());\n";
    } else if (scalar.logical_type.kind == ir::LogicalKind::FIXED_SIZE_BINARY) {
        out << ctx.indent << ctx.writer_expr 
            << ".WriteBinary(reinterpret_cast<const uint8_t*>(" 
            << final_expr << ".data()), " << scalar.logical_type.fixed_size_binary_width << ");\n";
    } else {
        // Use the positional write string from scalar_info
        out << ctx.indent << ctx.writer_expr << "." 
            << scalar_info.positional_write << "(" << final_expr << ");\n";
    }
}
```

Examples of emitted code shapes:

```text
// Non-nullable top-level scalar (uses .value_or(default)):
w.WriteInt32(field_.value_or(0));

// Nullable top-level scalar (uses *opt):
if (!field_.has_value()) w.SetNull(0);
else {
    w.WriteInt32(*field_);
}

// List element (raw value, no dereference):
{ auto lc = w.BeginList(static_cast<uint32_t>(field_.size()));
  for (uint32_t li_0 = 0; li_0 < field_.size(); ++li_0) {
      w.WriteInt32(field_[li_0]);  // RAW_VALUE access mode
  }
}

// Non-nullable struct (uses .value_or(...ctor)):
{ auto sw = w.BeginStruct(MyStructSchema()->n_children);
  field_.value_or(MyStruct()).EncodeStructTo_(sw);
}

// Nullable struct (uses *opt):
if (!field_.has_value()) w.SetNull(0);
else {
    auto sw = w.BeginStruct(MyStructSchema()->n_children);
    field_->EncodeStructTo_(sw);
}
```

List visitor:

```cpp
void EdgeEncodeVisitor::EmitList(const ir::ListNode& list,
                                 const ir::IrNode& node,
                                 const EncodeContext& ctx) {
    std::string loop_var = "li_" + std::to_string(ctx.depth);
    std::string final_expr = ctx.value_expr;
    
    if (ctx.access_mode == ValueAccessMode::DEREF_OPTIONAL) {
        final_expr = "*" + ctx.value_expr;
    } else if (ctx.access_mode == ValueAccessMode::VALUE_OR_DEFAULT) {
        // Lists are non-nullable at proto level; skip this case if it arises
        final_expr = ctx.value_expr;
    }
    
    out << ctx.indent << "{ auto lc = " << ctx.writer_expr 
        << ".BeginList(static_cast<uint32_t>(" << final_expr << ".size()));\n"
        << ctx.indent << "  for (uint32_t " << loop_var << " = 0; " 
        << loop_var << " < " << final_expr << ".size(); ++" << loop_var << ") {\n";
    
    auto elem_ctx = ctx;
    elem_ctx.value_expr = final_expr + "[" + loop_var + "]";
    elem_ctx.access_mode = ValueAccessMode::RAW_VALUE;
    elem_ctx.indent += "      ";
    elem_ctx.depth += 1;
    elem_ctx.top_level_field = false;
    
    EmitValue(*list.element, elem_ctx);
    
    out << ctx.indent << "  }\n}\n";
}
```

Struct visitor:

```cpp
void EdgeEncodeVisitor::EmitStruct(const ir::StructNode& st,
                                   const ir::IrNode& node,
                                   const EncodeContext& ctx) {
    std::string class_name = cpp_backend::CppClassName(st.identity.descriptor, ctx.context_file);
    std::string final_expr = ctx.value_expr;
    
    if (ctx.access_mode == ValueAccessMode::DEREF_OPTIONAL) {
        final_expr = "*" + ctx.value_expr;
    } else if (ctx.access_mode == ValueAccessMode::VALUE_OR_DEFAULT) {
        // Non-nullable struct: use .value_or(ctor)
        final_expr = ctx.value_expr + ".value_or(" + class_name + "())";
    }

    out << ctx.indent << "{ auto sw = " << ctx.writer_expr << ".BeginStruct("
        << class_name << "Schema()->n_children);\n"
        << ctx.indent << "  " << final_expr << ".EncodeStructTo_(sw);\n"
        << ctx.indent << "}\n";
}
```

Map visitor preserves current key/value order exactly:

```cpp
void EdgeEncodeVisitor::EmitMap(const ir::MapNode& map,
                                const ir::IrNode& node,
                                const EncodeContext& ctx) {
    std::string final_expr = ctx.value_expr;
    
    if (ctx.access_mode == ValueAccessMode::DEREF_OPTIONAL) {
        final_expr = "*" + ctx.value_expr;
    }
    // Maps are non-nullable at proto level; VALUE_OR_DEFAULT should not occur

    out << ctx.indent << "{ auto mc = " << ctx.writer_expr 
        << ".BeginMap(static_cast<uint32_t>(" << final_expr << ".size()));\n"
        << ctx.indent << "  for (const auto& [k, v] : " << final_expr << ") {\n";
    
    auto key_ctx = ctx;
    key_ctx.value_expr = "k";
    key_ctx.access_mode = ValueAccessMode::RAW_VALUE;
    key_ctx.indent += "      ";
    key_ctx.top_level_field = false;
    
    EmitValue(*map.key, key_ctx);
    
    out << ctx.indent << "  }\n"
        << ctx.indent << "  auto vc = mc.BeginValues();\n"
        << ctx.indent << "  for (const auto& [k, v] : " << final_expr << ") {\n";
    
    auto val_ctx = ctx;
    val_ctx.value_expr = "v";
    val_ctx.access_mode = ValueAccessMode::RAW_VALUE;
    val_ctx.indent += "      ";
    val_ctx.top_level_field = false;
    
    EmitValue(*map.value, val_ctx);
    
    out << ctx.indent << "  }\n}\n";
}
```

For message map values, value visitor emits struct encoding. For scalar map values, scalar visitor emits the scalar write.

`EmitEncodeTo()` changes shape from:

```cpp
for (size_t i = 0; i < fields.size(); ++i) EmitFieldEncode(o, fields[i], i);
```

to:

```cpp
for (size_t i = 0; i < fields.size(); ++i) {
    cpp_backend::EmitFieldEncodeFromIr(o, fields[i].ir, fields[i].name + "_", i, file);
}
```

For GIR-3, `FieldInfo` carries both forms until full migration:

```cpp
struct FieldInfo {
    std::string name;
    FieldMapping mapping;       // temporary bridge for old emitters
    ir::IrNode ir;              // new source of truth for encode
};
```

If owning/copying `IrNode` is awkward because of `unique_ptr`, use `std::shared_ptr<const ir::IrNode>` or value types with boxed children. Confirm no `std::vector<FieldInfo>` copy survives in `generator.cpp`.

The encode visitor must not use `FieldMapping` internally. If any part of encode calls `fi.mapping.kind`, the migration is incomplete.

### Unsupported node semantics

`Unsupported{reason}` is a typed IR node, not a mapper failure.

It means: "the field/message was analyzed successfully, and the generator knows why Fletcher cannot currently represent or emit it."

Examples:

```text
real oneof field
recursive message
proto2 group
google.protobuf.Any
google.protobuf.Struct
unsupported WKT
map key cannot map to scalar
map value cannot map
invalid flatten wrapper shape
required custom option/extension could not be interpreted
IR shape unsupported by a still-flat bridge projection
```

GIR-3 behavior:

- Unit tests assert `UnsupportedNode.reason`.
- Encode visitor emits a generation-time diagnostic path if reached.
- Existing generator behavior may still skip or comment unsupported fields through the old boundary, but the IR no longer uses `nullopt` as the representation.

GIR-8 behavior, not implemented now:

- `Unsupported{reason}` becomes a clean protoc build error via `AddError` / `*error`.
- Generated code should not contain silent `// TODO` or `.ValueOrDie()` generator shortcuts for unsupported mappings.

### Constraint checkpoints

STOP-AND-ASK if any proposed implementation embeds C++ or other backend type strings in IR nodes. C++ strings belong only in `cpp_backend::LookupScalar()` and backend descriptor helpers.

STOP-AND-ASK if dictionary is modeled as a structural IR container. Dictionary is a scalar modifier.

STOP-AND-ASK if enum descriptor/symbol identity is dropped. C++ encode may lower to `int32`, but the IR must retain enum identity.

STOP-AND-ASK if GIR-3 rewrites the RBA accessor emitter or deletes `FieldKind`. RBA is read-only in this round.

STOP-AND-ASK if generated wire bytes change for any currently supported fixture. GIR-2's parity oracle must remain green.

STOP-AND-ASK if the design removes the bespoke edge encoder or prevents GIR/BIND from adding a descriptor-driven ABI codec later. GIR-3 migrates bespoke edge encode only and must not foreclose the BIND-2 path.

STOP-AND-ASK if `MapField()` becomes a second independent classifier instead of a thin wrapper over `BuildFieldIr()` + `ProjectIrToFieldMapping()`. Both RBA and the encode visitor must consume the same IR source.

## Forcing-test mapping

Add a new unit translation unit:

```text
protoc/tests/test_ir.cpp
```

Wire it into:

```text
protoc/tests/CMakeLists.txt
```

by adding `test_ir.cpp` to `fletcher_proto_plugin_tests`.

Main forcing test:

```cpp
TEST(IrTest, BuildsLanguageNeutralIr)
```

Coverage cases:

1. Scalar logical identity:
   - Build descriptors for `bool`, `int32`, `int64`, `uint32`, `uint64`, `float`, `double`, `string`, `bytes`.
   - Assert `NodeKind::SCALAR`.
   - Assert `LogicalKind` values.
   - Assert nullable facts from `facts.nullable`.
   - Assert no C++ strings are present because the IR API exposes no such fields.

2. Enum identity:
   - Build enum `Color { RED = 0; GREEN = 1; }`.
   - Field `Color color = 1`.
   - Assert scalar logical kind is `INT32`.
   - Assert `enum_identity.descriptor->full_name()`.
   - Assert symbols include `RED=0`, `GREEN=1`.

3. WKT distinctions:
   - Use generated/descriptor dependencies for `google.protobuf.Timestamp`, `Duration`, and wrappers.
   - Assert timestamp is not a generic `INT64`; it is `WKT_TIMESTAMP` / `TIMESTAMP(NANO)`.
   - Assert duration is `WKT_DURATION` / `DURATION(NANO)`.
   - Assert wrappers become nullable scalar with the correct `WktKind::WRAPPER_*`.

4. Dictionary modifier:
   - Use whatever DICT annotation/spec hook exists when available.
   - Assert dictionary appears as `facts.dictionary == true`.
   - Assert it is not `NodeKind::MAP`, `STRUCT`, or `LIST`.
   - If DICT annotations are not yet in tree, keep the test helper at the IR-construction utility level so the invariant is covered without inventing public proto behavior.

5. Nesting:
   - `repeated int32` -> `List<Scalar(INT32)>`.
   - `message Inner { string value = 1; } Inner inner = 1` -> `Struct`.
   - `repeated Inner inner = 1` -> `List<Struct>`.
   - `map<string, int32>` -> `Map<Scalar(UTF8), Scalar(INT32)>`.
   - `map<string, Inner>` -> `Map<Scalar(UTF8), Struct>`.
   - Flatten wrapper chains -> nested `List` nodes.
   - Real oneof -> `Unsupported{reason}`.
   - Nested messages preserve `StructIdentity` descriptor/full name.

6. Unsupported reasons:
   - `google.protobuf.Any`.
   - `google.protobuf.Struct`.
   - Recursive message.
   - Proto2 group if descriptor construction allows.
   - Oneof.
   - Invalid map/flatten edge case.
   - Assert the reason is non-empty and specific.

7. C++ backend lookup:
   - Separate small tests may assert `cpp_backend::LookupScalar(LogicalKind::INT32)` returns `WriteInt32`, `arrow::int32()`, etc.
   - These tests prove strings live in C++ backend, not IR.

8. Single-source projection:
   - Assert `BuildFieldIr()` produces the same `IrNode` every time for the same field descriptor.
   - Assert `ProjectIrToFieldMapping(BuildFieldIr(...))` produces a deterministic `FieldMapping`.
   - Assert `MapField()` wrapper produces identical results to `ProjectIrToFieldMapping(BuildFieldIr(...))`.

Keep existing `test_type_mapper.cpp` green during the bridge period. It can continue testing `MapField()` and `FieldMapping` projection, but new behavior should be asserted in `test_ir.cpp`.

Encode oracle mapping:

```text
integration-tests/protoc-coverage/tests/test_parity_oracle.cpp
  ParityOracle.EncodeEqualsEncodeRowAndRoundTrips
```

must remain green after encode migration. That test asserts:

```text
row.Encode() == committed golden bytes
Codec.EncodeRow(ToArrowRow(row)) == committed golden bytes
row.Encode() == Codec.EncodeRow(ToArrowRow(row))
golden bytes decode to expected values
```

The IR-driven encoder is correct only if this oracle remains byte-identical.

**RBA no-drift golden** (new gate):

The RBA generated accessor code must produce byte-identical output after IR migration. Run:

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher\protoc
conan install . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default -DFLETCHER_BUILD_TESTS=ON
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "RecordBatchAccessorTest"
```

This ensures RBA output stays golden during IR projection changes.

**TS `tsc --noEmit` compilation check** (new gate):

The generated TypeScript interface must remain compilable after IR migration. In the coverage suite:

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher\integration-tests\protoc-coverage\out
tsc --noEmit *.ts
```

This ensures TS emitter input path (via IR projection) does not break the interface contract.

Inner-loop command shape (protoc unit tests + encode oracle):

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher\protoc
conan install . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default -DFLETCHER_BUILD_TESTS=ON
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "IrTest|TypeMapperTest|SchemaBuilder|RecordBatchAccessorTest"
```

Full GIR-3 regression check (all surfaces):

```powershell
# Protoc unit tests + RBA projection golden
cd C:\Users\CTM\source\prototypes\Fletcher\protoc
conan install . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default -DFLETCHER_BUILD_TESTS=ON
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "IrTest|TypeMapperTest|SchemaBuilder|RecordBatchAccessorTest"

# Coverage parity oracle (encode + decode round-trip)
cd C:\Users\CTM\source\prototypes\Fletcher\integration-tests\protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "ParityOracle"

# TS compilation check
cd C:\Users\CTM\source\prototypes\Fletcher\integration-tests\protoc-coverage\out
tsc --noEmit *.ts
```

A full GIR-3 cutover requires all three gates to pass: encode oracle byte-identity, RBA golden unchanged, TS `tsc --noEmit` compiles.

## Risks & Unknowns

The largest implementation risk is accidentally moving the old C++ string model into a new struct with a new name. That violates the central purpose of GIR. The IR must expose logical identity only; C++ strings must be backend lookup results.

The second risk is a divergence between `BuildFieldIr()` and the old `MapField()`, creating silent classification disagreement. To guard against this, `MapField()` must become a thin wrapper over IR + projection, not a second classifier. Coverage test case #8 asserts this by checking all three paths (IR direct, projection, and MapField wrapper) produce identical results.

Nested scalar lists are representable in the IR, but other generated surfaces may not yet support them. GIR-3 should not broaden fixture coverage into currently parked `coverage_future.proto` unless all emitted surfaces compile. If supporting encode for `List<List<Scalar>>` creates tension with the still-flat storage/API model, mark that specific projection unsupported and defer full activation to GIR-10.

The current generator stores C++ class references in `FieldMapping::nested_class` and `map_value_class`. The IR should store descriptors and proto identities; the C++ backend should compute class references. Any cross-file include collection that still needs C++ header names can use backend helpers or the temporary projection.

Map byte ordering is guarded by GIR-2. The IR map visitor must preserve current key-first/value-second emission and iteration order over generated storage. Do not normalize, sort, or rebuild map entries.

The current encode and decode helpers detect timestamp/duration by searching C++ Arrow type strings. GIR-3 must replace that with logical identity lookup for encode. Decode still uses the old helper until GIR-4, through the adapter.

`Unsupported{reason}` creates a better model before GIR-8's build-error behavior exists. Until GIR-8, generation boundaries may still behave like today for unsupported fields. The important GIR-3 invariant is that the IR no longer loses the reason.

RBA remains read-only and may continue consuming the old flat `FieldMapping` data via the IR projection until RIR. GIR-3 should avoid "cleaning up" RBA or chasing string parsing there.

Descriptor-driven ABI codec work is not part of GIR-3, but the IR and visitor design should leave a clean path for it. Do not bake assumptions into the IR that only bespoke generated C++ can consume.

## Files-to-touch

```text
protoc/include/ir.hpp
protoc/src/ir.cpp
```

New language-neutral IR data model and construction helpers.

```text
protoc/include/type_mapper.hpp
protoc/src/type_mapper.cpp
```

Add `BuildFieldIr()` / `BuildMessageIr()` APIs. Change `MapField()` to a thin wrapper over `BuildFieldIr()` + `ProjectIrToFieldMapping()`. Introduce `ProjectIrToFieldMapping()` as the canonical projection. Move scalar classification to logical identity.

```text
protoc/include/cpp_backend_type_table.hpp
protoc/src/cpp_backend_type_table.cpp
```

C++ backend lookup table keyed by IR logical type. Include `CppClassName()` helper for struct class name resolution.

```text
protoc/src/generator.cpp
```

Migrate edge encode emission to the IR recursive visitor. Keep decode/schema/view/TS on the bridge for this item. Update `FieldInfo` to carry IR plus temporary `FieldMapping` if needed.

```text
protoc/include/generator_internal.hpp
```

Update `FieldInfo` to carry IR plus temporary `FieldMapping` if needed.

```text
protoc/src/recordbatch_accessor_emitter.cpp
protoc/src/recordbatch_accessor_emitter.hpp
```

Do not rewrite. Touch only if required to consume the unchanged projected `FieldMapping` API via the IR. Otherwise leave read-only. Ensure RBA consumes `ProjectIrToFieldMapping(BuildFieldIr(...))` output, not any second classifier.

```text
protoc/tests/test_ir.cpp
protoc/tests/CMakeLists.txt
```

Add the GIR-3 forcing unit test (including case #8 for single-source verification) and wire it into the existing protoc test executable.

```text
protoc/tests/test_type_mapper.cpp
```

Keep existing bridge/projection tests green; adjust only where `MapField()` becomes a thin wrapper. Assert the equivalence of `MapField()`, `BuildFieldIr()`, and `ProjectIrToFieldMapping()` paths.

```text
integration-tests/protoc-coverage/tests/test_parity_oracle.cpp
```

No design-driven edits expected, but this test must remain green and is the byte-identity gate for the IR-driven encoder.

## Step-2 re-review (2026-07-10)

**Verdict: APPROVE.** All three prior blockers and the should-fix are resolved.
No locked-decision deviation; decision #1 (language-neutrality) is preserved — no
single-language type string leaked into the IR by any of the fixes. Two surgical
implementation-level corrections are required (both would be caught red by the
build gates, but flagging saves a cutover cycle); neither is architectural.

Prior findings — status:

1. **Blocker 1 (bridge single-source + drift gate): RESOLVED.** `BuildFieldIr()`
   is the one canonical classifier; `MapField()` is now a thin wrapper over
   `BuildFieldIr()` + `ProjectIrToFieldMapping()` (lines 405–412, 422); the
   "no second independent classifier" invariant and its STOP-AND-ASK guard are
   stated (line 826); test case #8 asserts the three paths agree. The forcing
   gate now includes the RBA no-drift golden (`-R RecordBatchAccessorTest`) and
   TS `tsc --noEmit *.ts`, with exact inner-loop commands and a "all three gates
   must pass" cutover rule (lines 926–983). Bridge-derived drift in RBA/TS is
   caught.

2. **Blocker 2 (optional-storage access): RESOLVED in substance.** The
   `ValueAccessMode` model (RAW_VALUE / DEREF_OPTIONAL / VALUE_OR_DEFAULT) is
   derived correctly for nullable vs non-nullable top-level and for
   container-interior positions across scalar/list/struct/map; the example
   emitted shapes (lines 622–648) are correct and default-write behavior is
   preserved (no byte-change risk; guarded by the encode oracle, which also fails
   to *build* if the `std::optional`-storage assumption is wrong). **Required fix
   (2a):** the DEREF_OPTIONAL snippets build `final_expr = "*" + value_expr` and
   then append `.member(...)`, emitting `*field_.EncodeStructTo_(sw)` (EmitStruct,
   line 703) and `*field_.data()/.size()` (EmitScalar BINARY/FSB, lines 606/610).
   Unary `*` binds looser than `.`, so these parse as `*(field_.member())` and do
   **not compile** for the reachable nullable-singular-message and nullable-bytes
   cases. Emit `field_->…` or `(*field_).…` (as the line-647 example already
   shows). RAW_VALUE and VALUE_OR_DEFAULT paths are unaffected (argument position
   / trailing `.value_or(...)`).

3. **Blocker 3 (single source of truth): RESOLVED in substance.** `nullable` and
   `dictionary` are removed from the node variants and centralized in
   `FieldFacts`; line 212 states `IrNode.facts` is the only home. **Required fix
   (3a):** `StructField` still carries its own `facts` (line 178) plus `name` /
   `field_number`, so a nested struct field holds two `FieldFacts` instances
   (`StructField.facts` and `StructField.type->facts`) and duplicates
   `proto_name`/`wire_field_id`. This is copy-of-same-data (same `BuildFieldIr`
   output, not a competing classifier), so it is not a drift *source*, but it
   contradicts the "`IrNode.facts` only" claim. Either drop `StructField.facts`
   (and the redundant name/number) and read `StructField.type->facts`, or state
   explicitly that `StructField.facts` is a non-authoritative alias of
   `type->facts`.

4. **Should-fix 4 (Arrow/TS derivability): RESOLVED.** Line 263 asserts every
   `LogicalKind` (incl. `WKT_TIMESTAMP`/`WKT_DURATION`, enum-as-`INT32`, and
   dictionary-modified scalars) has a unique Arrow physical type and TS
   `WireTypeId` via per-backend tables, so GIR-5/7 are not forced into an IR
   re-design. Minor: dictionary lives in `facts.dictionary` (not `LogicalType`),
   so the Arrow/TS type is derived from `LogicalType` + `facts.dictionary` +
   `enum_identity` — all in the IR, language-neutrally — rather than from
   `LogicalKind` alone. The doc's wording is slightly loose but the data is
   complete and no backend string is required to recover a logical fact.

Nothing regressed against `docs/robustness-plan.md` §2a/§2b or
`GIR-locked-decisions.md` #1/#3/#4/#7/#8.
