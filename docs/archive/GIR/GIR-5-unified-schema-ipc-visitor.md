# GIR-5 Unified Schema + IPC Visitor Design

## Summary

GIR-5 unifies the schema-source and in-process/IPC schema code paths onto one IR-driven visitor by extracting the flatten walk from `GatherFieldsImpl` into a shared helper and defining a `SchemaSink` abstraction that both the C++-source renderer and in-process nanoarrow executor implement. This eliminates duplicated schema construction logic while guaranteeing byte-for-byte agreement between generated source and `.ipc` output through `.ipc` golden-file comparisons.

Both `GenerateSchemaFunction` and `BuildMessageSchemaInto` are replaced with calls to a single visitor:

```cpp
SchemaVisitor visitor(message_descriptor, context_file);
visitor.EmitCppSchemaFunction(out, class_name);     // C++ source sink, yields <Class>Schema()
visitor.BuildIpcSchema(schema.get());               // nanoarrow sink, yields .ipc output
```

The visitor walks a flattened field list and emits the same logical operations (set type, set name, set nullable, set metadata) through two different sinks. Any future schema change touches the visitor once, not two code paths. Locked decision #5 requires both paths to migrate together in this item.

## Design

### Architecture: Visitor, Flatten Walk, Sink Abstraction

The implementation has three core pieces:

1. **Flatten Walk** (`BuildFlattenedFieldList`): A `FieldMapping`-free walk that reproduces `GatherFieldsImpl`'s field-level flatten logic (descending into singular wrapper fields) and builds the dotted `field_id` path. This walk reads only from the IR, not from descriptor-specific metadata, and is language-neutral.

2. **Visitor** (`SchemaVisitor`): Orchestrates the flatten walk, iterates over flattened fields, and calls sink operations (SetType, SetName, SetNullable, SetMetadata, DeepCopyMessageStruct) in the correct order.

3. **Sink Abstraction** (`SchemaSink`): An abstract interface that encapsulates all nanoarrow calls and child navigation. Both `CppSchemaSink` (renders source text) and `NanoarrowSchemaSink` (executes nanoarrow calls) implement it identically.

### Flatten Walk Extraction

The flatten walk must be extracted into a shared helper function:

```cpp
struct SchemaFieldRecord {
    std::string name;
    int32_t field_number = 0;
    std::string field_id;  // dotted path, e.g. "2.1"
    std::shared_ptr<const ir::IrNode> node;
};

// Walk the message descriptor, applying field-level flatten and building the
// flattened field list and dotted field_id paths. Same logic as GatherFieldsImpl,
// but for schema-walk purposes only (no FieldMapping projection).
std::vector<SchemaFieldRecord> BuildFlattenedFieldList(
    const google::protobuf::Descriptor* msg,
    const std::string& id_prefix = "");
```

The implementation mirrors `GatherFieldsImpl` exactly:
- For each field in `msg`, build its IR node and path.
- If the field's IR node is `UNSUPPORTED` or `FIXED_SIZE_LIST`, skip it (do not add to result), matching today's `ProjectIrToFieldMapping` filter at type_mapper.cpp:150-152.
- If the field is a singular (non-repeated) message with field-level flatten (checked via `HasFieldFlatten(fd)`), recurse into it with the extended path and skip the field itself.
- Otherwise, add the field to the result with its leaf `field_number` and computed `field_id`.

The walk is language-neutral: it reads only proto descriptor + IR node, not type-mapper state. Rationale for filtering: surfacing unsupported→build error is **GIR-8 (#55)** per locked decision #6, and enabling FIXED_SIZE_LIST is later scope—both are OUT OF GIR-5. GIR-5 is a byte-identical schema migration only.

### Sink Abstraction and Child Navigation

`SchemaRef` is a `void*` opaque handle to an `ArrowSchema*`:

```cpp
using SchemaRef = void*;

class SchemaSink {
public:
    virtual ~SchemaSink() = default;

    // Initialize the root struct and allocate `child_count` children.
    virtual void InitRootStruct(SchemaRef root, int64_t child_count) = 0;

    // Navigate to child `i` of a struct. Used after SetType calls that allocate
    // children. Returns the SchemaRef for the child.
    virtual SchemaRef Child(SchemaRef parent, int i) = 0;

    // Map-specific: return the entries struct of a map, then key/value children.
    // These are derived from nanoarrow MAP structure (entries -> [key, value]).
    virtual SchemaRef MapEntries(SchemaRef map_parent) = 0;
    virtual SchemaRef MapKey(SchemaRef entries) = 0;
    virtual SchemaRef MapValue(SchemaRef entries) = 0;

    // Set the Arrow type and allocate children if needed.
    virtual void SetTypeScalar(SchemaRef schema, ArrowType type) = 0;
    virtual void SetTypeList(SchemaRef schema) = 0;
    virtual void SetTypeStruct(SchemaRef schema, int64_t child_count) = 0;
    virtual void SetTypeMap(SchemaRef schema) = 0;

    // Set the scalar type for timestamp/duration (includes time unit and timezone).
    virtual void SetTypeDateTime(SchemaRef schema, ArrowType type,
                                 ArrowTimeUnit time_unit,
                                 const char* timezone) = 0;

    // Set the field name.
    virtual void SetName(SchemaRef schema, std::string_view name) = 0;

    // Set the nullable flag (true to set ARROW_FLAG_NULLABLE, false to clear it).
    virtual void SetNullable(SchemaRef schema, bool nullable) = 0;

    // Set metadata pairs (e.g., {{"field_number", "1"}, {"field_id", "2.1"}}).
    virtual void SetMetadata(SchemaRef schema,
                             std::span<const std::pair<std::string, std::string>> pairs) = 0;

    // Deep-copy the schema produced by a nested message's schema path and
    // place it at `dst`. Used for nested struct fields; the visitor then
    // applies the field overlay (name, nullable, metadata) after the copy.
    // For the C++ source sink, this emits the DeepCopy call as text; for the
    // nanoarrow sink, this invokes the recursive visitor.
    virtual void DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                                       SchemaRef dst) = 0;
};
```

### CppSchemaSink: Text Renderer

The `CppSchemaSink` renders nanoarrow calls as C++ source lines:

```cpp
class CppSchemaSink : public SchemaSink {
private:
    std::ostringstream& out_;
    std::string indent_;
    SchemaVisitor* visitor_;  // for recursive nested-message schema calls

public:
    CppSchemaSink(std::ostringstream& out, const std::string& indent, SchemaVisitor* visitor)
        : out_(out), indent_(indent), visitor_(visitor) {}

    void InitRootStruct(SchemaRef root, int64_t child_count) override {
        auto* p = static_cast<const char*>(root);
        out_ << indent_ << "ArrowSchemaInit(" << p << ");\n"
             << indent_ << "ArrowSchemaSetTypeStruct(" << p << ", " << child_count << ");\n";
    }

    SchemaRef Child(SchemaRef parent, int i) override {
        auto* p = static_cast<const char*>(parent);
        // Return a string expression (as void*) that will be cast back later.
        // For the source renderer, we build expressions like "schema->children[0]".
        return static_cast<void*>(new std::string(std::string(p) + "->children[" + 
                                                   std::to_string(i) + "]"));
    }

    // ... other SetType* methods emit the corresponding ArrowSchemaSetType calls
};
```

### NanoarrowSchemaSink: In-Process Executor

The `NanoarrowSchemaSink` executes nanoarrow calls in-process:

```cpp
class NanoarrowSchemaSink : public SchemaSink {
private:
    SchemaVisitor* visitor_;  // for recursive nested-message schema calls

public:
    explicit NanoarrowSchemaSink(SchemaVisitor* visitor) : visitor_(visitor) {}

    void InitRootStruct(SchemaRef root, int64_t child_count) override {
        auto* schema = static_cast<ArrowSchema*>(root);
        ArrowSchemaInit(schema);
        CheckNa(ArrowSchemaSetTypeStruct(schema, child_count), "set struct type");
    }

    SchemaRef Child(SchemaRef parent, int i) override {
        auto* schema = static_cast<ArrowSchema*>(parent);
        return static_cast<void*>(schema->children[i]);
    }

    // ... other methods execute the real nanoarrow calls
};
```

### SchemaVisitor: Orchestration

```cpp
class SchemaVisitor {
private:
    const google::protobuf::Descriptor* message_;
    const google::protobuf::FileDescriptor* context_file_;
    std::unique_ptr<SchemaSink> sink_;

public:
    SchemaVisitor(const google::protobuf::Descriptor* msg,
                  const google::protobuf::FileDescriptor* context_file,
                  std::unique_ptr<SchemaSink> sink)
        : message_(msg), context_file_(context_file), sink_(std::move(sink)) {}

    void Visit() {
        // Build the flattened field list once, use for both paths.
        auto fields = BuildFlattenedFieldList(message_);

        // Root struct initialization
        SchemaRef root = ...;  // sink-specific
        sink_->InitRootStruct(root, fields.size());

        // Root metadata
        std::vector<std::pair<std::string, std::string>> root_meta = {
            {"proto_package", message_->file()->package()},
            {"proto_message", message_->name()},
        };
        sink_->SetMetadata(root, root_meta);

        // For each flattened field: emit type, then overlay (name, nullable, metadata)
        for (size_t i = 0; i < fields.size(); ++i) {
            const auto& field = fields[i];
            SchemaRef child = sink_->Child(root, i);

            EmitNodeType(field.node.get(), child);  // SetType* calls
            sink_->SetName(child, field.name);
            sink_->SetNullable(child, field.node->facts.nullable);

            std::vector<std::pair<std::string, std::string>> field_meta = {
                {"field_number", std::to_string(field.field_number)},
                {"field_id", field.field_id},
            };
            sink_->SetMetadata(child, field_meta);
        }
    }

private:
    void EmitNodeType(const ir::IrNode* node, SchemaRef schema) {
        switch (node->kind) {
            case NodeKind::SCALAR:
                EmitScalarType(std::get<ScalarNode>(node->node), schema);
                break;

            case NodeKind::LIST:
                sink_->SetTypeList(schema);
                {
                    auto& list_node = std::get<ListNode>(node->node);
                    SchemaRef item = sink_->Child(schema, 0);
                    EmitNodeType(list_node.element.get(), item);
                    sink_->SetName(item, "item");  // Restore after element type (esp. deep-copy)
                }
                break;

            case NodeKind::STRUCT: {
                auto& struct_node = std::get<StructNode>(node->node);
                sink_->DeepCopyMessageStruct(struct_node.identity.descriptor, schema);
                break;
            }

            case NodeKind::MAP: {
                auto& map_node = std::get<MapNode>(node->node);
                sink_->SetTypeMap(schema);
                SchemaRef entries = sink_->MapEntries(schema);
                SchemaRef key_child = sink_->MapKey(entries);
                SchemaRef value_child = sink_->MapValue(entries);

                // Key is always scalar (nanoarrow map constraint)
                EmitScalarType(std::get<ScalarNode>(map_node.key->node), key_child);

                // Value: scalar or struct
                if (map_node.value->kind == NodeKind::STRUCT) {
                    auto& val_struct = std::get<StructNode>(map_node.value->node);
                    sink_->DeepCopyMessageStruct(val_struct.identity.descriptor, value_child);
                    sink_->SetName(value_child, "value");
                } else {
                    EmitScalarType(std::get<ScalarNode>(map_node.value->node), value_child);
                    // Scalar "value" gets default nanoarrow name, don't override
                }
                break;
            }
        }
    }

    void EmitScalarType(const ir::ScalarNode& scalar, SchemaRef schema) {
        // Derive nanoarrow type from LogicalType + optional EnumIdentity
        const auto& logical_type = scalar.logical_type;

        if (logical_type.kind == LogicalKind::BOOL) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_BOOL);
        } else if (logical_type.kind == LogicalKind::INT32) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_INT32);
        } else if (logical_type.kind == LogicalKind::INT64) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_INT64);
        } else if (logical_type.kind == LogicalKind::UINT32) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_UINT32);
        } else if (logical_type.kind == LogicalKind::UINT64) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_UINT64);
        } else if (logical_type.kind == LogicalKind::FLOAT32) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_FLOAT);
        } else if (logical_type.kind == LogicalKind::FLOAT64) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_DOUBLE);
        } else if (logical_type.kind == LogicalKind::UTF8) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_STRING);
        } else if (logical_type.kind == LogicalKind::BINARY) {
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_BINARY);
        } else if (logical_type.kind == LogicalKind::TIMESTAMP) {
            // Timestamp unit is hardcoded to NANO in today's emitter
            sink_->SetTypeDateTime(schema, NANOARROW_TYPE_TIMESTAMP,
                                   NANOARROW_TIME_UNIT_NANO, nullptr);
        } else if (logical_type.kind == LogicalKind::DURATION) {
            // Duration unit is hardcoded to NANO in today's emitter
            sink_->SetTypeDateTime(schema, NANOARROW_TYPE_DURATION,
                                   NANOARROW_TIME_UNIT_NANO, nullptr);
        } else if (logical_type.kind == LogicalKind::WKT_TIMESTAMP) {
            sink_->SetTypeDateTime(schema, NANOARROW_TYPE_TIMESTAMP,
                                   NANOARROW_TIME_UNIT_NANO, nullptr);
        } else if (logical_type.kind == LogicalKind::WKT_DURATION) {
            sink_->SetTypeDateTime(schema, NANOARROW_TYPE_DURATION,
                                   NANOARROW_TIME_UNIT_NANO, nullptr);
        } else if (scalar.enum_identity) {
            // Enum physical type is always INT32, regardless of LogicalKind
            sink_->SetTypeScalar(schema, NANOARROW_TYPE_INT32);
        } else {
            throw std::runtime_error("unsupported scalar logical kind in schema visitor");
        }
    }
};
```

### Public API Entry Points

```cpp
// protoc/include/cpp_backend_schema_visitor.hpp
namespace fletcher::cpp_backend {

// Generate the C++ source text for <Class>Schema().
std::string GenerateSchemaFunctionFromIr(
    const std::string& class_name,
    const google::protobuf::Descriptor* msg,
    const google::protobuf::FileDescriptor* context_file);

// Build the in-process nanoarrow schema for a message (same as old BuildMessageSchemaInto).
void BuildMessageSchemaIntoFromIr(
    const google::protobuf::Descriptor* msg,
    ArrowSchema* schema);

}  // namespace fletcher::cpp_backend
```

The first entry point creates a `CppSchemaSink`, visits the message, and returns the generated source. The second creates a `NanoarrowSchemaSink`, visits the message, and populates the `ArrowSchema*`.

### Nested Message Struct Handling

When emitting a nested struct field, the visitor calls `sink_->DeepCopyMessageStruct(nested_descriptor, child_schema)`. This operation has two sinks:

**CppSchemaSink** emits the source call:
```cpp
ArrowSchemaDeepCopy(<NestedClass>Schema().get(), schema->children[i]);
```

**NanoarrowSchemaSink** recursively invokes the visitor on the nested message:
```cpp
BuildMessageSchemaIntoFromIr(nested_descriptor, child_schema);
```

For repeated struct (list of struct), the visitor emits `SetTypeList`, then recursively emits the struct element (which calls `DeepCopyMessageStruct`), then restores the `"item"` name after the deep-copy. For nested lists (list of list of struct), the visitor iterates the list depth first, then calls `DeepCopyMessageStruct` on the innermost child, then restores the final `"item"` name. This matches today's behavior in generator.cpp:973-996.

### Metadata Content and Order

Root schema metadata is `{{"proto_package", <pkg>}, {"proto_message", <msg>}}` in that order.

Field metadata is `{{"field_number", <num>}, {"field_id", <path>}}` in that order.

The field overlay applies SetType calls first, then SetName, then SetNullable, then SetMetadata. This order is critical for nested structs: the deep-copy establishes the nested struct's name and metadata, then SetName overwrites the name to match the parent field, and SetMetadata replaces the metadata with field-number/field_id.

## Forcing-test mapping

The forcing test `SchemaVisitor.CppAndIpcByteIdentical` in `protoc/tests/test_schema_visitor.cpp` is the internal-consistency check: it verifies that both schema paths produce identical serialized output. However, this test **cannot** alone guard against drift from today because once unified onto one visitor, agreement is by construction.

The **real drift guard** is per-node-kind `.ipc` golden-file comparison. For each of these scenarios, commit the current-day `.ipc` file as a golden:

1. **Field-level flatten** (dotted `field_id`): A message with a singular wrapper field that is flattened; verify the flattened leaf field appears with `field_id` "2.1" (or appropriate path).

2. **Nested struct** (metadata replacement): A message with a nested struct field; verify the struct's `proto_message` metadata is overwritten by the field overlay to be absent (replaced by field-number/field_id).

3. **Repeated struct** (item naming): A message with a repeated struct field; verify the list child is named `"item"` and retains the nested struct's `proto_message` metadata.

4. **Map with scalar value**: A message with a map field where the value is scalar; verify the map structure and default `"value"` name.

5. **Map with struct value** (value naming and metadata): A message with a map field where the value is a struct; verify the struct value is renamed `"value"` and retains `proto_message` metadata.

6. **Nested list with struct leaf**: A message with `List<List<Struct>>`; verify the list layers are named `"item"` and the innermost struct retains `proto_message`.

7. **WKT timestamp/duration**: A message with WKT `google.protobuf.Timestamp` or `google.protobuf.Duration`; verify the schema type is `NANOARROW_TYPE_TIMESTAMP` or `NANOARROW_TYPE_DURATION` with `NANOARROW_TIME_UNIT_NANO`.

8. **Proto2 optional**: A message with a proto2 `optional` field; verify the field is marked `ARROW_FLAG_NULLABLE`.

9. **Dictionary**: A message with a dictionary-encoded field; verify the schema emits the physical scalar type (not dictionary-specific changes).

10. **Enum**: A message with an enum field; verify the schema type is `NANOARROW_TYPE_INT32` (enum lowered to int, symbol emission deferred to GIR-9).

Each golden is the verbatim `.ipc` file serialized from the current emitter (captured and checked into the repo). The integration test harness rebuilds the plugin, runs protoc on golden-test protos with `--fletcher_opt=ipc`, and compares each generated `.ipc` file byte-for-byte against its golden. Any drift in serialized schema bytes is flagged immediately.

The source-code golden (generated C++ `<Class>Schema()` text) may move under review as scaffolding or emitter polish changes, but the `.ipc` goldens are immutable and prove fidelity.

### Test Dependencies

`SchemaVisitor.CppAndIpcByteIdentical` depends on the Phase-3a compile-and-run harness for `CompileAndRunGeneratedClassSchema`. This function compiles the generated header with a test `main()` that invokes `<Class>Schema()` and returns the result. The test harness must exist or this test is deferred until Phase-3a lands.

## Risks & Unknowns

**Flatten walk correctness** is the main risk. The new walk must reproduce `GatherFieldsImpl`'s field-level flatten behavior exactly, including the dotted `field_id` path computation, and must filter `UNSUPPORTED` and `FIXED_SIZE_LIST` nodes (matching type_mapper.cpp:150-152). The walk is tested implicitly by the `.ipc` golden comparisons, but should also be unit-tested in isolation on a few nested-wrapper fixtures.

**Nanoarrow defaults** (e.g., list item nullability, map child names, struct initialization) must be tested through serialized IPC bytes. Small changes to call order or missing `SetName` calls can produce binary-different `.ipc` files that are caught by the golden comparison.

**Nested message schema recursion** must ensure that both sinks (source and in-process) produce identical child schemas. The in-process sink recurses through the visitor; the source sink calls the nested schema function. Both must yield identical serialized bytes.

**Dictionary modifier** is preserved on the IR (`FieldFacts::dictionary`), but today the schema path ignores it and emits the physical scalar type. GIR-5 continues this behavior. If schema-source and IPC paths diverge on dictionary handling, that is a stop-and-ask to reconcile before implementation (locked decisions #5 and #7 require one visitor).

**Enum representation** remains `INT32 + EnumIdentity` (symbol emission is GIR-9). The schema visitor correctly routes enum to INT32 type. No symbol metadata is emitted in schema for this item.

**Unsupported and fixed-size-list filtering** is out of scope for GIR-5: both node kinds are filtered by the flatten walk (matching type_mapper.cpp:150-152), so they are unreachable in the visitor. Surfacing unsupported→build error is **GIR-8 (#55)** per locked decision #6; enabling FIXED_SIZE_LIST is later scope. GIR-5 preserves today's byte-identical behavior.

## Files-to-touch

`protoc/include/cpp_backend_schema_visitor.hpp`: public API for `GenerateSchemaFunctionFromIr` and `BuildMessageSchemaIntoFromIr`; declarations of `SchemaVisitor`, `SchemaSink`, `CppSchemaSink`, `NanoarrowSchemaSink`, `SchemaFieldRecord`, and `BuildFlattenedFieldList`.

`protoc/src/cpp_backend_schema_visitor.cpp`: implementation of flatten walk, visitor orchestration, both sinks, and public entry points.

`protoc/include/generator_internal.hpp`: document that `GatherFields` entry point is now read-only for schema visitor (no changes needed, but clarify intent).

`protoc/src/generator.cpp`: replace the bodies of `GenerateSchemaFunction` and `BuildMessageSchemaInto` with calls to `GenerateSchemaFunctionFromIr` and `BuildMessageSchemaIntoFromIr`. Keep public `BuildMessageSchema` and plugin `.ipc` emission (no change, it already calls `BuildMessageSchemaInto`). Do not remove `GatherFieldsImpl`, `EmitNanoarrowTypeSetup`, or `SetScalarSchemaType` yet (they may be reused by other emitters during incremental migration).

`protoc/include/schema_builder.hpp`: update the comment to note that `BuildMessageSchema` is now the execution sink of the IR-driven schema visitor used by generated source.

`protoc/tests/test_schema_builder.cpp`: keep existing tests green unchanged. No new assertions needed beyond the existing coverage.

`protoc/tests/test_schema_visitor.cpp` (new file): add `SchemaVisitor.CppAndIpcByteIdentical` and per-node-kind `.ipc` golden-file tests.

Integration test harness (under `protoc/tests` or reuse existing compile-and-run location): add `.ipc` golden-file comparison. Before running tests, force a protoc plugin rebuild. After rebuild, run protoc on golden-test protos with `--fletcher_opt=ipc` and compare generated `.ipc` files against committed goldens byte-for-byte.

## Clarifications

**Green guard, not red-first**: Per locked decision #9, GIR-5 is a migration item guarded by a green oracle, not red-first. `SchemaVisitor.CppAndIpcByteIdentical` is green before and after—the two old paths are already hand-kept in lockstep today. Its real value is catching source-render-vs-direct-exec divergence during future changes. The frozen `.ipc` goldens are the primary anti-drift guard; they are captured from today's emitter and must not drift unless explicitly reviewed and approved.

**Unsupported-field policy**: Today the schema path skips unsupported fields (filtered by `GatherFieldsImpl` via `ProjectIrToFieldMapping` nullopt at type_mapper.cpp:150-152). The schema visitor inherits this behavior—unsupported nodes are filtered by `BuildFlattenedFieldList`, so they never reach the visitor and the plugin succeeds. This aligns with today's behavior and defers unsupported→build-error to GIR-8/#55 (locked decision #6).

**API shape**: Both member methods (`SchemaVisitor(msg, context_file, sink)` + `Visit()`) and free functions (`GenerateSchemaFunctionFromIr(cls, msg, context_file)` returning the source, and `BuildMessageSchemaIntoFromIr(msg, schema)` populating the ArrowSchema) are supported. The free functions internally create the visitor and sink. The `context_file` is threaded through the sink for the C++ source path to resolve cross-file qualified class names.

**Timestamp/duration hardcoding**: The time unit is hardcoded to `NANOARROW_TIME_UNIT_NANO` today. The visitor must NOT inspect `logical_type.time_unit` (even though it may be present in the IR). Always emit NANO. This is a locked implementation detail to prevent drift.

**Enum representation**: An enum is `INT32` logical kind (not a separate `ENUM` kind, which does not exist in `LogicalKind`). The visitor routes `INT32 + enum_identity` to `NANOARROW_TYPE_INT32`. Symbol metadata is GIR-9 scope.

**Warning comments**: Today `GenerateSchemaFunction` emits `// Warning: <msg>` comments from `fi.mapping.warning`. The source golden may reflect these comments. The schema visitor does not carry warnings into the IR, so this feature is dropped for generated source unless the IR is extended to carry warnings (GIR-8 scope). The `.ipc` golden is unaffected (binary output unchanged).

**Map child names**: Nanoarrow allocates the map structure with default child names `"entries"`, `"key"`, and `"value"`. The visitor does **not** call `SetName` on the scalar key or value (respecting nanoarrow defaults). It **does** call `SetName("value")` only for struct values, to override the name after deep-copy. This matches today's behavior exactly.

**Metadata order**: Root metadata is `proto_package` then `proto_message`. Field metadata is `field_number` then `field_id`. Metadata builders are order-preserving; insertion order is serialization order. Tests must verify order via serialized `.ipc` bytes.

**Field-level flatten correctness**: The flatten walk must exactly reproduce `GatherFieldsImpl`'s recursion and path building, and must filter `UNSUPPORTED` and `FIXED_SIZE_LIST` nodes (matching type_mapper.cpp:150-152 and generator.cpp ~847-872). It is tested implicitly by `.ipc` golden-file comparisons and explicitly (if needed) by unit tests on a few nested-wrapper fixtures. The walk is a schema-local mirror of `GatherFieldsImpl` logic, not a refactoring of the original.

**Dictionary as modifier**: The IR carries `FieldFacts::dictionary`, but the schema visitor ignores it and emits the physical scalar type (matching today's behavior). If a future migration requires dictionary-specific schema emission, that must land in BOTH sinks via the same visitor in a single patch, or escalated (per locked #5 and #7).

**RBA read-only**: GIR-5 must not rewrite RBA (`GenerateAccessorGettersClass` / `EmitRecordBatchAccessors`), must not delete `FieldKind`, and must not alter accessor outputs except for baseline movement from shared generator scaffolding. `GatherFields` is called read-only by the accessor emitter.

**BIND-2 codec constraint**: Locked decision #8 requires descriptor-driven codec options to remain open. GIR-5 only owns schema construction and IPC schema bytes. It must not remove or constrain the bespoke edge-codec path or defer codec decisions to the schema visitor. Codec-specific metadata is GIR-8 or BIND-2 scope.

## Step-2 review (2026-07-10) — final re-review

**Verdict: APPROVE.** The two defects left open by the prior re-review are resolved,
and nothing regressed in the previously-blessed parts. Checked against live code.

1. **Flatten-walk filter — RESOLVED.** `BuildFlattenedFieldList` skips
   `UNSUPPORTED` and `FIXED_SIZE_LIST` via a language-neutral `node.kind` check
   (doc §Flatten Walk Extraction, line 51; §Risks line 404; §Clarifications
   line 442). This matches `type_mapper.cpp:150-152` (`case NodeKind::UNSUPPORTED:
   case NodeKind::FIXED_SIZE_LIST: return std::nullopt;`) and the skip-and-succeed
   behaviour of `GatherFieldsImpl` (`generator.cpp:847-872`, nullopt → skipped
   comment, plugin succeeds). No throw-on-unsupported-field (the only `throw` is the
   defensive unmappable-scalar fallback in `EmitScalarType`, mirroring today's
   `SetScalarSchemaType` at `generator.cpp:1186`); `EmitNodeType`'s switch has no
   `FIXED_SIZE_LIST` emit branch (unreachable post-filter). Doc correctly homes
   unsupported→build-error in GIR-8/#55 per locked #6 → preserves today's bytes,
   not a stop-and-ask.

2. **LIST "item" naming — RESOLVED.** `EmitNodeType`'s LIST branch recurses the
   element FIRST, then restores `SetName(item, "item")` AFTER (lines 226-234).
   Correct for all three shapes: List<Scalar> (scalar `SetType` never touches the
   name; restore is consistent, matches `generator.cpp:935-970`), List<Struct>
   (deep-copy overwrites the name, restore fixes it, matches `973-979`), and
   List<List<Struct>> (recursion restores "item" at each layer and after the
   innermost deep-copy, matches `981-996`). The design's parent-type-before-child
   mutation order yields the same final in-memory tree as today, so `.ipc`
   serialized bytes are identical (source golden may move per line 386).

**Spot-check of blessed parts — no regressions:**
- **flatten + field_id**: dotted `field_id` path + leaf `field_number` mirror
  `GatherFieldsImpl` (`HasFieldFlatten` recursion, `id_prefix` path build).
- **sink abstraction + per-node calls**: MAP branch (SetTypeMap → entries →
  key/value, struct-value `SetName("value")`, scalar-value no rename) matches
  `generator.cpp:998-1063`; field overlay order (type → name → nullable →
  metadata{field_number,field_id}) and root metadata order
  ({proto_package, proto_message}) match `generator.cpp:1079-1129`.
- **decision #5** both paths on one visitor; **#1** language-neutral IR
  (`node.kind` filter, LogicalKind→nanoarrow lives in the C++ sink, not the IR);
  **#3** RBA read-only / `FieldKind` retained; **#4** schema reads IR directly,
  not the temporary `FieldMapping` bridge. All honoured.
- The 10 enumerated `.ipc` goldens remain the immutable anti-drift guard.

No genuine unresolved blockers. Minor illustrative-code notes (e.g. `CppSchemaSink::Child`
returning `new std::string` as `void*` in the sketch) are pseudo-code, not blocking,
and out of scope for this final pass.
