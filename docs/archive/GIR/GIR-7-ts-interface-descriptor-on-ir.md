# GIR-7 TypeScript Interface + SchemaDescriptor IR Visitor

## Summary

GIR-7 migrates the TypeScript interface and runtime `TypedSchema` / `SchemaDescriptor` generator from the flat `FieldInfo` / `FieldMapping` switches to a direct recursive IR visitor. The migration is byte-identity guarded against today's generated TypeScript, including the exact interface names, field type text, `WireTypeId` names, descriptor object shape, imports, service topic constants, skipped recursive messages, and current parked syntax-error behavior.

This is the first non-C++ backend table in the GIR rewrite. It proves locked decision #1: IR scalar nodes carry only abstract logical-type identity, never TypeScript strings. TypeScript type text and `WireTypeId` names live only in `ts_backend`, keyed by `ir::LogicalType` plus optional `ir::EnumIdentity`. A future Rust or C# backend consumes the same IR through its own backend table.

The current emitter behavior to preserve is defined in:

```text
protoc/src/type_mapper.cpp:
  TsScalarType
  TsInterfaceName

protoc/src/generator.cpp:
  TsFieldType
  TsWireTypeId
  EmitTsFieldDescriptor
  GenerateTsMessage
  GenerateTypeScriptFile
```

Today's active coverage output includes:

```ts
bool_value: boolean;
int32_value: number;
int64_value: bigint;
string_value: string;
bytes_value: Uint8Array;
optional_bool: boolean | null;
wrapped_bool: boolean | null;
timestamp_value: bigint;
duration_value: bigint;
status: number;
repeated_scalar: number[];
repeated_struct: ILeaf[];
nested_struct_lists: ILeaf[][];
depth3_struct_lists: ILeaf[][][];
map_scalar: Map<string, number>;
map_struct: Map<string, ILeaf>;
flattened_struct_list: IStructListWrapper[];
optional_flattened_struct_list: IStructListWrapper[] | null;
```

Descriptor literals must remain in today's shape:

```ts
{ name: 'repeated_struct', fieldNumber: 8, wireType: WireTypeId.LIST, nullable: false, element: { name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT, nullable: false, fields: Leaf.fields } },
{ name: 'flattened_struct_list', fieldNumber: 12, wireType: WireTypeId.LIST, nullable: false, element: { name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT, nullable: false, fields: StructListWrapper.fields } },
```

## Design

Add a TypeScript backend layer beside the existing C++ backend tables:

```cpp
namespace fletcher::ts_backend {

struct TsScalarInfo {
    std::string ts_type_text;      // "boolean", "number", "bigint", "string", "Uint8Array"
    std::string wire_type_id;      // "WireTypeId.BOOL", "WireTypeId.INT32", ...
};

const TsScalarInfo& TsLookupScalar(
    const ir::LogicalType& type,
    const std::optional<ir::EnumIdentity>& enum_identity);

std::string TsInterfaceName(const google::protobuf::Descriptor* msg);
std::string TsSchemaConstName(const google::protobuf::Descriptor* msg);

}  // namespace fletcher::ts_backend
```

`TsLookupScalar` replaces both today's `TsScalarType` and the scalar half of `TsWireTypeId`. The table maps IR logical identity to TypeScript text and descriptor wire type:

```cpp
BOOL         -> { "boolean",    "WireTypeId.BOOL" }
INT32        -> { "number",     "WireTypeId.INT32" }
INT64        -> { "bigint",     "WireTypeId.INT64" }
UINT32       -> { "number",     "WireTypeId.UINT32" }
UINT64       -> { "bigint",     "WireTypeId.UINT64" }
FLOAT32      -> { "number",     "WireTypeId.FLOAT32" }
FLOAT64      -> { "number",     "WireTypeId.FLOAT64" }
UTF8         -> { "string",     "WireTypeId.STRING" }
BINARY       -> { "Uint8Array", "WireTypeId.BINARY" }
WKT_TIMESTAMP/NANO -> { "bigint", "WireTypeId.TIMESTAMP_NANO" }
WKT_DURATION/NANO  -> { "bigint", "WireTypeId.DURATION_NANO" }
```

Enums stay storage-compatible with today's output: the IR scalar has `LogicalKind::INT32` plus `EnumIdentity`, and the TS table returns `{ "number", "WireTypeId.INT32" }`. `EnumIdentity` is preserved for GIR-9 / future symbol emission, but GIR-7 must not change emitted TS enum field text.

`TsInterfaceName` preserves today's naming exactly: prepend `I`, and for nested messages concatenate containing message names with `_`. Examples: `ICompositeCoverage`, `IBranch_Leaf`.

Add a visitor class:

```cpp
namespace fletcher::ts_backend {

class TsVisitor {
public:
    explicit TsVisitor(const google::protobuf::FileDescriptor* file);

    std::string GenerateFile();

private:
    std::string GenerateMessage(const google::protobuf::Descriptor* msg);
    std::string InterfaceType(const ir::IrNode& node,
                              const google::protobuf::Descriptor* declared_struct = nullptr);
    std::string DescriptorLiteral(const ir::IrNode& node,
                                  std::string_view name,
                                  int32_t field_number,
                                  const google::protobuf::Descriptor* declared_struct,
                                  std::string_view indent);

    std::string ElementDescriptor(const ir::IrNode& node,
                                  const google::protobuf::Descriptor* declared_struct);
    std::string MapKeyDescriptor(const ir::IrNode& node);
    std::string MapValueDescriptor(const ir::IrNode& node);

    const google::protobuf::FileDescriptor* file_;
};

}  // namespace fletcher::ts_backend
```

**Message walk:** Use `cpp_backend::BuildFlattenedFieldList` to obtain the flattened field list, which applies field-level flatten inlining and drops unsupported fields. Each `SchemaFieldRecord` carries `name`, `field_number`, `field_id` (dotted path), and `node` (shared IR):

```cpp
const auto fields = cpp_backend::BuildFlattenedFieldList(msg);
if (fields.empty()) return "";  // Skip empty messages

emit "export interface I<M> {";
for each record in fields:
    declared = DeclaredWrapperFor(record.source_field);  // see Wrapper-name recovery below
    emit "  <record.name>: " + InterfaceType(*record.node, declared) + ";";
emit "}";

emit "export const <M>: TypedSchema<I<M>> = {";
emit "  fields: [";
for each record in fields:
    declared = DeclaredWrapperFor(record.source_field);  // NOT nullptr — threads #12/#18 wrapper name
    emit DescriptorLiteral(*record.node, record.name, record.field_number, declared, "    ");
emit "  ],";
emit "  protoPackage: '<package>',";
emit "  protoMessage: '<msg->name()>',";
emit "};";
```

**Wrapper-name recovery:** For singular flatten-wrapper list fields (e.g., `StructListWrapper flattened_struct_list = 12`), the flattened IR is `List<Struct(Leaf)>`, but the TS output must preserve the declared wrapper name `IStructListWrapper[]` / `StructListWrapper.fields` (not the leaf `ILeaf[]` / `Leaf.fields`). 

The IR node alone cannot carry the wrapper name (locked decision #1); instead, recover it by consulting the source proto descriptor. Extend `SchemaFieldRecord` to carry an optional `source_field` pointer:

```cpp
struct SchemaFieldRecord {
    std::string name;
    int32_t field_number = 0;
    std::string field_id;
    std::shared_ptr<const ir::IrNode> node;
    const google::protobuf::FieldDescriptor* source_field = nullptr;  // [NEW]
};
```

In `BuildFlattenedFieldListImpl`, set `source_field = fd` when recording a non-flattened field. For flattened field inlining (when we recurse), `source_field = nullptr` (inlined fields have no single source field). 

In the TS visitor, when rendering a `List<Struct>` node where `source_field` is set and points to a message type `M` with a single repeated field (i.e., `IsFlattenedWrapper(M)` is true), use the declared wrapper name: 

```cpp
std::string GetDeclaredStructName(const ir::StructNode& st,
                                  const google::protobuf::Descriptor* source_field_msg) {
    // If source_field_msg is a flattened wrapper, return its name (the declared wrapper).
    // Otherwise, return the struct node's identity name (the inlined type).
    if (source_field_msg && IsFlattenedWrapper(source_field_msg)) {
        return source_field_msg->name();
    }
    return st.identity.descriptor->name();
}
```

Pass `declared_struct` through the visitor pipeline: when emitting the outer list field, inspect `record.source_field` and if it's a singular message with `IsFlattenedWrapper`, pass that message descriptor to `InterfaceType` / `DescriptorLiteral` as `declared_struct`. For inlined fields, pass `nullptr`. The rendering methods emit the declared name when present, otherwise the flattened IR name.

**Per-node rendering:**

```text
Scalar:
  interface: TsLookupScalar(type).ts_type_text + nullable suffix
  descriptor: { name, fieldNumber, wireType: TsLookupScalar(type).wire_type_id, nullable }

List<T>:
  interface: InterfaceType(T, declared_struct) without top-level nullable + "[]" + nullable suffix
  descriptor: { wireType: WireTypeId.LIST, nullable, element: ElementDescriptor(T, declared_struct) }
    where element always has nullable: false (no recursive nullable in descriptors)

FixedSizeList<T, n>:
  same TS and descriptor rendering as List<T>; size is not surfaced in today's TS descriptor

Struct:
  interface: TsInterfaceName(struct.identity.descriptor or declared_struct) + nullable suffix
  descriptor: { wireType: WireTypeId.STRUCT, nullable, fields: <SchemaConst>.fields }
    where SchemaConst is derived from the struct identity (flattened) or declared wrapper name

Map<K, V>:
  interface: Map<InterfaceType(K, nullptr), InterfaceType(V, nullptr)> + nullable suffix
  descriptor: { wireType: WireTypeId.MAP, nullable, mapKey: ..., mapValue: ... }

Unsupported:
  preserve current skipped/parked behavior; do not fix GIR-10 cases here
```

The nullable suffix is interface-only and applies to the top-level field type exactly as today:

```ts
optional_bool: boolean | null;
optional_leaf: ILeaf | null;
optional_flattened_struct_list: IStructListWrapper[] | null;
```

Nested descriptor element nodes always use today's anonymous shape with hardcoded `nullable: false`:

```ts
{ name: '', fieldNumber: 0, wireType: WireTypeId.INT32, nullable: false }
```

Nested lists are built recursively from the outside in and must preserve today's byte output:

```ts
{ name: 'depth3_struct_lists', fieldNumber: 16, wireType: WireTypeId.LIST, nullable: false, element: { name: '', fieldNumber: 0, wireType: WireTypeId.LIST, nullable: false, element: { name: '', fieldNumber: 0, wireType: WireTypeId.LIST, nullable: false, element: { name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT, nullable: false, fields: Leaf.fields } } } },
```

**File generation:** `TsVisitor::GenerateFile()` orchestrates the full file, preserving today's byte-identical output:

1. **File header:** Emit the standard comment and imports (TypedSchema and WireTypeId).

2. **Cross-file imports:** Scan all messages in `OrderedMessages(file)` for struct/list/map fields whose message type lives in a different file. For each cross-file dependency, emit an import line: `import { /* cross-file types */ } from './<proto_file>.fletcher.js';`. Placeholder imports stay as-is (cross-file symbol resolution is GIR-8 scope).

3. **Message emission in dependency order:** Use `OrderedMessages(file)` to walk messages (ensures parents emit before children). For each message:
   - If `IsRecursive(msg)`, emit skip comment: `// Skipped: <msg.name> is recursive and cannot be represented.\n`
   - Otherwise, call `GenerateMessage(msg)` and emit its interface + schema descriptor.

4. **Service method topic constants:** Iterate `file->service_count()` and `svc->method_count()`. For each method, validate with `ValidateServiceMethod(method, generated_msgs, &reason)`. If valid, emit topic constant:
   ```ts
   export const <Service>_<Method>Topic = '<package>/<service>/<method>';
   ```
   If invalid, emit skip comment: `// Skipped: <Service>.<Method> — <reason>\n` (e.g., "request is not streaming (pub/sub requires 'stream' on request)").

5. **Empty-message skip:** `GenerateMessage` returns `""` when the flattened field list is empty; the file generator skips emitting (no interface/descriptor output for empty messages).

**Decision #1 proof point:** No `boolean`, `number`, `bigint`, `Uint8Array`, `WireTypeId.*`, `Map<...>`, or `I...` TypeScript text is stored on `ir::IrNode`, `ir::ScalarNode`, `ir::LogicalType`, or `ir::FieldFacts`. The IR stays language-neutral. TS text exists only in `ts_backend`.

**Bridge state after GIR-7:**

```text
Edge encode/decode: direct IR emitters, no FieldMapping
Schema + IPC: direct IR emitters, no FieldMapping
View + ToArrowRow: direct IR emitters, no FieldMapping
TS: direct IR emitter, no FieldMapping
RBA C++/Rust: remains on thin IR -> FieldMapping projection, read-only
Edge row-class setters/getters: consume FieldMapping from the shared projection
C++-source schema path (ArrowTypeExpr/EmitNanoarrowTypeSetup): consume FieldMapping, deprecated post-GIR-5 (now uses IR)
```

`FieldMapping` survives as a read-only thin projection (used by RBA and edge row-class emitters) until RIR. GIR-7 narrows it by dropping the TS consumer only — it is not RBA-only until RIR. `GatherFields`, `ProjectIrToFieldMapping`, and the bridge infrastructure remain intact for RBA.

## Forcing-Test Mapping

Add `TsVisitor.DescriptorByteIdentical`.

**Location:** `protoc/tests/test_ts_visitor.cpp` (preferred) or integration harness.

**Baseline golden:** Commit today's generated TS file (`integration-tests/protoc-coverage/build/generated/coverage.fletcher.ts`) as a frozen golden under `integration-tests/protoc-coverage/golden/coverage.fletcher.ts`. The build and test harness must NOT overwrite this golden; the test harness diffs against it. Regen path: env gate (e.g., `FLETCHER_REGEN_GOLDEN=1`) triggers a one-time regen after review approval of semantic parity.

**Test approach:**

```text
1. Generate .fletcher.ts for every active fixture through the new TsVisitor path.
2. Compare byte-for-byte against the committed golden.
3. On failure, report a unified diff.
4. Keep parked TS syntax-error cases unchanged; GIR-10 owns those.
```

The test must guard:

```text
interface names: I<M>, nested IParent_Child
scalar TS text: boolean / number / bigint / string / Uint8Array
nullable suffix: " | null"
WKT text: Timestamp and Duration -> bigint
enum text: number
repeated scalar: T[]
repeated struct: ILeaf[]
nested lists: ILeaf[][] / ILeaf[][][]
maps: Map<string, number> / Map<string, ILeaf>
flatten-wrapper lists: IStructListWrapper[] (not ILeaf[]), fields: StructListWrapper.fields (not Leaf.fields)
descriptor wireType names
descriptor nullable booleans
element / fields / mapKey / mapValue literal shape
protoPackage / protoMessage
imports and topic constants
recursive-message skip comments
```

Run `tsc --noEmit` in the integration harness after generating TS files (if tsc is available; skip as accepted if absent). Generated TS must type-check for supported active fixtures.

**Full-suite GIR-7 checkpoint:**

```text
protoc unit rebuild: --build=fletcher-protoc/*
coverage harness: C++ compile, Arrow view, TS generation + tsc, IPC schema golden
RBA accessor tests: no drift (FieldMapping read-only, no change to edge row-class emitters)
round-trip tests: green
whole component/integration suite: green
```

## Risks & Unknowns

**Byte identity** is the main risk. Descriptor literals must be formatted exactly (single-line with exact spacing and trailing commas). Reuse the same formatting helpers from the current emitter or deliberate byte-shape copies.

**Flatten-wrapper name recovery** requires careful coordination. The source FieldDescriptor pointer in SchemaFieldRecord must be set correctly: non-null for top-level fields, null for inlined (field-level-flattened) fields. Test this explicitly: #12 (singular flatten wrapper) must emit `IStructListWrapper[]`/`StructListWrapper.fields`; #14 (repeated flatten wrapper) must emit `ILeaf[][]`/`Leaf.fields` (no wrapper name for repeated); #18 (optional singular flatten wrapper) must emit `IStructListWrapper[] | null`.

**Cross-file imports** remain intentionally weak. GIR-7 preserves the placeholder `/* cross-file types */` and does not invent symbol resolution (GIR-8 scope).

**Enum identity** must not alter TS output. Enums remain `number` and `WireTypeId.INT32`; symbolic enum emission is GIR-9 scope.

**Nested scalar-list and known TS syntax-error fixes** are GIR-10 scope. GIR-7 preserves current behavior.

**Descriptor-driven codec work** must remain visible. GIR-7 only migrates TS generation; it must not silence or bypass the descriptor-driven codec path (locked decision #8).

## Files to Touch

```text
protoc/include/ts_backend_type_table.hpp
protoc/src/ts_backend_type_table.cpp
```

New TS backend scalar/WKT/enum lookup table (mirrors cpp_backend_type_table.hpp).

```text
protoc/include/ts_backend_visitor.hpp
protoc/src/ts_backend_visitor.cpp
```

TS visitor class. Uses `cpp_backend::BuildFlattenedFieldList` and decorates with declared-wrapper name recovery.

```text
protoc/include/cpp_backend_schema_visitor.hpp
protoc/src/cpp_backend_schema_visitor.cpp
```

Extend `SchemaFieldRecord` with optional `source_field` pointer. Update `BuildFlattenedFieldList` / `BuildFlattenedFieldListImpl` to set `source_field = fd` for non-flattened fields, `source_field = nullptr` for inlined fields.

```text
protoc/src/generator.cpp
```

Replace the TS-specific `FieldInfo` / `FieldMapping` implementation with calls into `ts_backend::TsVisitor`, while preserving `GenerateTypeScriptFile` output bytes exactly.

```text
protoc/src/type_mapper.cpp
```

Retain `TsScalarType` and `TsInterfaceName` as-is (may be reused by other callers during transition). Mark as deprecated if no longer consumed by generator.cpp.

```text
protoc/tests/test_ts_visitor.cpp
protoc/tests/CMakeLists.txt
```

Add `TsVisitor.DescriptorByteIdentical` test.

```text
integration-tests/protoc-coverage/golden/coverage.fletcher.ts
integration-tests/protoc-coverage/CMakeLists.txt
```

Commit frozen golden. Wire `tsc --noEmit` through the harness.

## Step-2 review (2026-07-10)

Verdict: NEEDS-REWORK.

**Blocking:**

1. **Use `cpp_backend::BuildFlattenedFieldList`, not `ir::BuildMessageIr(msg).fields`.** The pseudocode in the original design contradicts the doc's own flatten prose. `BuildMessageIr` iterates raw `msg->field_count()` and does NOT apply field-level flatten and does NOT drop unsupported fields — diverging from today's `GatherFields` and breaking `FieldFlattenedPosition` (must inline `x`,`y`). Only `BuildFlattenedFieldList` reproduces the inlining + drop set. Read each `SchemaFieldRecord{name, field_number, node}` where `field_number` is the leaf `fd->number()` (matching today's TS).

2. **Singular flatten-wrapper list fields (#12, #18) must preserve the declared wrapper name.** For `flattened_struct_list` and `optional_flattened_struct_list`, today's TS emits `IStructListWrapper[]` / `fields: StructListWrapper.fields` (the declared wrapper), not `ILeaf[]` / `Leaf.fields` (the flattened inner type). Since the flattened IR node is `List<Struct(Leaf)>` with identity pointing to Leaf, a naive visitor would emit the leaf name — a byte-identity break. Recover the wrapper name by extending `SchemaFieldRecord` with an optional `source_field` pointer, set for non-flattened top-level fields. In the TS visitor, when rendering a `List<Struct>` where `source_field` points to a `IsFlattenedWrapper` message, emit the declared wrapper name instead of the flattened struct identity.

**Should-fix:**

3. **Byte-identity baseline hygiene.** The golden must be a committed file (e.g., `integration-tests/protoc-coverage/golden/coverage.fletcher.ts`), not a `build/` artifact. The test diffs against the golden; regen only via env gate after review.

4. **`GenerateFile` details.** Specify message ordering (via `OrderedMessages`), recursive-skip comments, cross-file import scan and placeholder emission, service topic constants and skip comments (e.g., "request is not streaming"), empty-message skip, and nested element/mapKey/mapValue hardcoded `nullable: false`.

5. **Bridge-retirement claim is overstated.** "After GIR-7 FieldMapping survives only as the RBA projection" is inaccurate. Edge row-class setters/getters consume `fi.mapping.*`, and the deprecated C++-source schema path still uses `FieldMapping` (now superseded by IR in GIR-5). Correct: FieldMapping narrows by dropping the TS consumer only; RBA + edge row-class retain it. It is not RBA-only until RIR.

---

**REWORK COMPLETE.** This fresh design incorporates all three blockers and all three should-fixes. The flatten-walk fix (#1) and wrapper-name recovery (#2) are the load-bearing changes; the others are clarity and hygiene.

## Step-2 re-review (2026-07-10)

Verdict: **APPROVE.**

Checked the reworked doc against the real emitter (`generator.cpp` TS path,
`BuildFlattenedFieldList`/`BuildFlattenedFieldListImpl` in
`cpp_backend_schema_visitor.cpp`, the flatten resolvers in `ir.cpp`, and the live
golden `coverage.fletcher.ts`). Both prior blockers and all three should-fixes are
resolved:

1. **Blocker 1 (flatten walk) — RESOLVED.** Message walk now uses
   `cpp_backend::BuildFlattenedFieldList(msg)` throughout (field-level flatten
   inlining via `HasFieldFlatten` + `IsSchemaRepresentable` drops), with
   `field_number = leaf fd->number()`. Verified this reproduces today's field set
   for `CompositeCoverage`/`FieldFlattenedPosition` (x,y inlined at fieldNumber
   1/2; no dropped-field divergence). No more `BuildMessageIr(msg).fields`.
2. **Blocker 2 (wrapper-name byte-identity) — RESOLVED.** #12/#18 recover the
   declared `StructListWrapper` name via `record.source_field` gated on
   *singular* `IsFlattenedWrapper`; #14/#16 stay `ILeaf[][]`/`Leaf.fields` because
   their source field is *repeated* (declared=nullptr). The name comes from
   `Descriptor::name()` (a language-neutral proto fact), not the Leaf IR identity,
   and not a re-baseline. Discriminator (singular vs repeated) matches today's
   REPEATED_STRUCT-vs-NESTED_LIST split exactly. No TS/C++ string on any IR node
   (decision #1 preserved; enum stays INT32 + EnumIdentity).
3. **Should-fix 3 — RESOLVED.** Golden is a committed frozen file
   (`integration-tests/protoc-coverage/golden/coverage.fletcher.ts`) with
   `FLETCHER_REGEN_GOLDEN` env gate; not a `build/` artifact.
4. **Should-fix 4 — RESOLVED.** `GenerateFile` now specifies OrderedMessages
   ordering, recursive-skip comment (correct TS variant, no "in Arrow"),
   cross-file placeholder imports, `ValidateServiceMethod` topic constants +
   skip reason, empty-message skip, and nested `nullable:false`. (The TS walk
   correctly does NOT skip `IsFlattenedWrapper` messages, unlike the C++ path —
   matches the golden emitting `StructListWrapper`/`NestedStructListWrapper`.)
5. **Should-fix 5 — RESOLVED.** Bridge state now states FieldMapping stays a
   read-only thin projection for RBA **and** edge row-class emitters until RIR;
   GIR-7 drops only the TS consumer. Consistent with locked #3/#4.

Locked decisions #1, #3/#4, #7, #8 honored; no deviation → no stop-and-ask.
Parked scalar-leaf flatten cases (fields 11/13/15/17 in `coverage_future.proto`)
remain **unfixed** (GIR-10), as required.

Non-blocking (fixed inline / for the implementer):
- The "Message walk" pseudocode hardcoded `declared_struct = nullptr`, which
  contradicted the wrapper-recovery prose and (if taken literally) would drop the
  #12/#18 wrapper name. Fixed inline to thread `DeclaredWrapperFor(record.source_field)`.
- Doc says inlined (field-level-flatten) fields get `source_field = nullptr`; a
  naive recursion sets it to the inner `fd`. Harmless for the fixture (inlined
  fields are scalars, so recovery never triggers), but the implementer should keep
  recovery gated on *singular message + IsFlattenedWrapper* regardless.
- Cross-file import scan walks raw `msg->field_count()` today; the flattened walk
  could differ in exotic cross-file cases. Cross-file is intentional GIR-8
  placeholder scope, so not load-bearing for GIR-7's golden.
