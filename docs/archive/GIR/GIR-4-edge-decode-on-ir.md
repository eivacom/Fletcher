# GIR-4: Edge Decode IR Visitor

## Summary

GIR-4 migrates only the generated edge C++ decode path onto the language-neutral IR introduced by GIR-3. The affected surface is the edge decoder code: generated `Foo(const uint8_t*, size_t)` / `Foo(PositionalReader&)` constructors that consume positional format, extract scalars/structs/lists/maps into generated members using sequential reads, and reconstruct the message in-place. The old `FieldMapping` decode path remains available until GIR-2's decode round-trip oracle is green with the IR-driven decoder.

The hard invariant is byte and behavior identity. GIR-4 must not change encoded wire bytes, decoded values, null behavior, field order, positional read order, map ordering, list cardinality handling, or memory ownership. The coverage harness remains the cutover gate: newly encoded bytes must round-trip through the generated edge constructor unchanged, and committed golden bytes must decode identically.

After GIR-4, direct IR emitters are:

```text
edge C++ encode
edge C++ decode (from PositionalReader)
```

The temporary IR-to-`FieldMapping` bridge still feeds:

```text
schema/IPC
Arrow view + field extraction (SetFromScalars_)
TypeScript interface + descriptor
RBA C++ + Rust accessor
```

The bridge retirement sequence remains:

```text
GIR-4 decode done
GIR-5 schema/IPC done
GIR-6 view done
GIR-7 TS done
RIR RBA done, then FieldKind/FieldMapping retire
```

## Design

### Decode visitor shape

Add an edge decode visitor beside the GIR-3 encode visitor. It consumes `ir::IrNode` directly and uses the C++ backend lookup for C++ scalar type strings and positional read method names. The IR remains language-neutral: no C++ type names, reader method names, default expressions, or storage spellings are added to IR nodes.

```cpp
namespace fletcher::cpp_backend {

enum class DecodeTargetMode {
    RAW_VALUE,           // container element or local variable
    OPTIONAL_STORAGE,    // generated std::optional<T> member storage
    VALUE_STORAGE,       // generated non-optional local/member storage, if present
};

struct DecodeContext {
    std::ostringstream& out;

    std::string reader_expr;       // e.g. "r", "lr", "sr", "vr"
    std::string target_expr;       // e.g. "id_", "(*field_).x_", "elem"
    std::string field_index_expr;  // top-level positional index for IsNull(), e.g. "0"
    std::string indent;

    DecodeTargetMode target_mode;
    bool top_level_field;
    bool nullable_position;

    int depth;
    NameSource* names;  // fresh list/map/struct loop and local names

    const google::protobuf::FileDescriptor* context_file;
};

class EdgeDecodeVisitor {
public:
    explicit EdgeDecodeVisitor(DecodeContext ctx);

    void EmitField(const ir::IrNode& node);
    void EmitValue(const ir::IrNode& node, const DecodeContext& ctx);

private:
    void EmitScalar(const ir::IrNode& node, const DecodeContext& ctx);
    void EmitList(const ir::IrNode& node, const DecodeContext& ctx);
    void EmitStruct(const ir::IrNode& node, const DecodeContext& ctx);
    void EmitMap(const ir::IrNode& node, const DecodeContext& ctx);
    void EmitUnsupported(const ir::IrNode& node, const DecodeContext& ctx);
};

}  // namespace fletcher::cpp_backend
```

`DecodeContext` mirrors GIR-3's `EncodeContext`: it threads the current target storage expression, current reader expression, indentation, fresh loop names, and backend context. The decode visitor tracks target storage mode because extraction writes into generated members.

GIR-3's `ValueAccessMode` remains the encode-side model for optional dereference/default handling. GIR-4 must reuse the same optional-storage contract, but on the assignment side:

```text
nullable scalar/struct top-level storage:
  null bit set    -> target_.reset()
  non-null value  -> target_.emplace(decoded_value) or target_ = decoded_value

non-nullable scalar/struct optional-backed storage:
  decoded value   -> target_ = decoded_value
  do not synthesize a different default path unless old decode already did so

container element:
  decode into a local raw value, then push/emplace into the container
```

The invariant is the same generated storage behavior for every shape the current generator emits. GIR-4 is a migration of `EmitFieldDecode`, not a rewrite of optional/nullable semantics.

### Scalar lowering

Scalar decode uses `cpp_backend::LookupScalar(logical_type, enum_identity)` to determine the PositionalReader method name via logic equivalent to `PositionalReadCall` (generator.cpp L1538-1555), which derives the method based on `storage_type` and `arrow_type_expr`: checking for `ReadTimestamp` or `ReadDuration` via expr.find(), distinguishing `ReadBinary` from `ReadString` via expr equality check, and mapping remaining storage types to their corresponding Read* methods. The derived method name is used for sequential value reads with the appropriate null checks and ownership semantics.

Representative shape for nullable optional-backed int32:

```cpp
if (!r.IsNull(0)) {
    id_ = r.ReadInt32();
}
```

For non-nullable int32:

```cpp
id_ = r.ReadInt32();
```

For nullable binary (returns pair<const uint8_t*, size_t>), with owned string storage:

```cpp
if (!r.IsNull(1)) {
    auto [p, n] = r.ReadBinary();
    data_.emplace(reinterpret_cast<const char*>(p), n);
}
```

For non-nullable string, with owned storage:

```cpp
label_ = std::string(r.ReadString());
```

The generated decoder copies strings/bytes into owned `std::string` members; there is no zero-copy borrow hazard on the edge decode path. Strings and binary are owned copies, not views.

Enums remain first-class in the IR but lower through the current C++ physical path until GIR-9:

```cpp
status_ = r.ReadInt32();
```

No enum symbol strings are added to the IR. C++ enum spelling remains a backend concern.

### Struct lowering

Struct decode calls `r.ReadStruct(NestedSchema()->n_children)` to open a sub-reader, then constructs the nested message in-place from that reader. It preserves existing constructor-based extraction.

Representative top-level non-nullable struct:

```cpp
{ auto sr = r.ReadStruct(Address::Schema()->n_children);
  address_.emplace(sr); }
```

Nullable struct must check the null bit before opening the sub-reader:

```cpp
if (!r.IsNull(3)) {
    auto sr = r.ReadStruct(Address::Schema()->n_children);
    address_.emplace(sr);
}
```

Nested struct fields follow the same null-check pattern:

```cpp
if (!sr.IsNull(1)) {
    auto child_reader = sr.ReadStruct(Child::Schema()->n_children);
    parent_->child_.emplace(child_reader);
}
```

The visitor computes C++ class names from descriptor identity through `cpp_backend::CppClassName()`.

### List lowering

List decode reads list metadata via `r.ReadListHeader()`, then iteratively decodes elements into the container. The list header holds the element count; nested lists recurse through the same pattern.

Representative scalar list of int32:

```cpp
{ auto lh = r.ReadListHeader();
  scores_.clear();
  scores_.reserve(lh.count);
  for (uint32_t li_ = 0; li_ < lh.count; ++li_) {
    scores_.push_back(r.ReadInt32());
  } }
```

Nullable/optional flatten-wrapped list must respect the outer null bit before touching the inner list:

```cpp
if (!r.IsNull(5)) {
    auto lh = r.ReadListHeader();
    points_.emplace();
    points_->clear();
    points_->reserve(lh.count);
    for (uint32_t li_ = 0; li_ < lh.count; ++li_) {
        auto sr = r.ReadStruct(FlattenedPoint::Schema()->n_children);
        points_->emplace_back(sr);
    }
}
```

Nested list decoding must be truly recursive with fresh loop-variable names for each depth. The current emitter supports struct-innermost nested lists using `resize()` and indexed assignment. For `List<List<Struct>>`:

```cpp
{ auto lh_0 = r.ReadListHeader();
  matrix_.resize(lh_0.count);
  for (uint32_t i_0 = 0; i_0 < lh_0.count; ++i_0) {
    auto lh_1 = r.ReadListHeader();
    matrix_[i_0].resize(lh_1.count);
    for (uint32_t i_1 = 0; i_1 < lh_1.count; ++i_1) {
      auto sr = r.ReadStruct(Struct::Schema()->n_children);
      matrix_[i_0][i_1] = Struct(sr);
    }
  } }
```

The reader cursor advances sequentially through nested reads; each list's `ReadListHeader()` consumes metadata and positions the cursor for the next element read.

This is a decode-emitter migration, not a coverage expansion. Shapes that the current generated code does not already emit remain unchanged until later coverage items.

### Fixed-size list lowering

Fixed-size list decode is **currently unsupported** and remains deferred. The current `EmitFieldDecode` generator has no FixedSizeList case. If GIR-4 analysis reveals that the IR's FixedSizeList node should map to an existing emitter path, that is a coverage expansion requiring separate analysis and design; flag before implementing.

### Map lowering

Map decode must preserve the existing Arrow map layout and extraction order: keys first, values second. The sequential cursor is kept in sync via the map-value bitfield.

Correct shape for `map<string, int32_t>`:

```cpp
{ auto count = r.ReadMapCount();
  std::vector<std::string> keys_;
  keys_.reserve(count);
  for (uint32_t mi_ = 0; mi_ < count; ++mi_) {
    keys_.emplace_back(r.ReadString());
  }

  auto vbf = r.ReadMapValueBitfield(count);
  settings_.clear();
  settings_.reserve(count);
  for (uint32_t mi_ = 0; mi_ < count; ++mi_) {
    settings_.emplace_back(std::move(keys_[mi_]), r.ReadInt32());
  } }
```

The `r.ReadMapValueBitfield(count)` call is **critical**: it consumes the value presence bitmap from the wire. Skipping it desynchronizes the cursor and corrupts all subsequent field reads.

Map storage is a `std::vector<std::pair<KeyType, ValueType>>`, preserved via sequential `emplace_back`. This maintains duplicate-key behavior and iteration order exactly as encoded.

For map values that are structs:

```cpp
{ auto count = r.ReadMapCount();
  std::vector<std::string> keys_;
  keys_.reserve(count);
  for (uint32_t mi_ = 0; mi_ < count; ++mi_) {
    keys_.emplace_back(r.ReadString());
  }

  auto vbf = r.ReadMapValueBitfield(count);
  by_id_.clear();
  by_id_.reserve(count);
  for (uint32_t mi_ = 0; mi_ < count; ++mi_) {
    auto sr = r.ReadStruct(Leaf::Schema()->n_children);
    by_id_.emplace_back(std::move(keys_[mi_]), Leaf(sr));
  } }
```

The key and value child nodes are visited recursively with raw target locals. The map node itself controls the key-first/value-second extraction order and the bitfield synchronization.

### Unsupported nodes

`Unsupported{reason}` remains an analyzed IR node. GIR-4 does not implement GIR-8 build-error behavior, but the decode visitor must not silently emit incorrect code for unsupported nodes.

Acceptable GIR-4 behavior:

```text
unsupported node reaches decode visitor
  -> generation diagnostic or existing unsupported boundary
  -> no silent TODO decode body that corrupts data
```

### Decode ownership contract

The edge decoder **copies** strings and binary data into owned `std::string` members. The PositionalReader is consumed sequentially during construction; there is no zero-copy borrow hazard on the edge decode path.

Example:

```cpp
// Binary reads return pair<const uint8_t*, size_t>
auto [p, n] = r.ReadBinary();
data_.emplace(reinterpret_cast<const char*>(p), n);  // copies into owned std::string
```

The decoded message owns all its data. This is a property of the edge constructor, not the test harness. Do not change string/binary ownership to views without re-evaluating the full decode lifetime contract and the bridge's use of these members.

## Forcing-test mapping

The forcing test is:

```text
integration-tests/protoc-coverage/tests/test_coverage_harness.cpp
  CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs (line 88)
```

This test exercises the edge decode constructor directly:

```cpp
const gen::CompositeCoverage decoded(encoded);
```

For GIR-4, the test must:

1. Round-trip every active fixture and committed golden bytes through the generated edge constructor.
2. Verify that all decoded scalar/struct/list/map fields match the encoded source.
3. Confirm that nullable fields have correct presence and values.

The test is currently scoped to `CompositeCoverage` from freshly-encoded bytes. Extend it to cover:

- Every fixture in `coverage_fixture.hpp` (not just `CompositeCoverage`).
- Committed golden bytes (loaded from disk and decoded via the edge constructor).
- Round-trip equivalence: encode → decode via constructor → re-encode, confirming byte equality.

This is the true red-first test: it fails if the edge decoder is incorrect, and passes when GIR-4 migration completes.

The parity oracle (`test_parity_oracle.cpp`) decodes via `Codec::DecodeRow` + Arrow `View`, which is the **Arrow-view / `SetFromScalars_` surface (GIR-6)**. Keep it green as a non-regression check, but do not claim it guards edge decode; it never calls the generated edge constructor.

Commands for the inner loop:

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher\protoc
conan install . --build=missing -pr:a=../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default -DFLETCHER_BUILD_TESTS=ON
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "CoverageHarness"
```

GIR-4 uses the same three-gate cutover model as GIR-3 because the bridge still feeds RBA and TS:

```text
1. Coverage harness green with IR-driven decoder (all fixtures + golden bytes).
2. RBA no-drift gate green, because RBA still consumes projected FieldKind.
3. TS compile gate green, because TS still consumes the bridge until GIR-7.
```

The protoc unit suite (`test_ir.cpp`) is part of the inner loop and must be green before relying on the three cutover gates.

## Risks & Unknowns

A wire-byte change is a stop-and-ask. GIR-4 changes decode emission only; it must not change encode output, schema layout, field order, null bitmap semantics, list/map layout, or golden bytes.

If the IR-driven decode migration reveals a genuine existing decode bug that corrupts data today, such as loss of precision, an off-by-one positional read, a missing null-bit check, incorrect map key/value synchronization, or map-layout misalignment, flag it as a finding and stop before baking in a silent behavior change. Per locked decision #6, fixes for shipping wire corruption land with the guarded baseline. Any fix that changes wire bytes is a stop-and-ask.

Nullable struct and nullable optional-wrapped list extraction are syntax-risk areas. Decode emission must emit the null check before opening the sub-reader or list header. Failure to do so leaves decoded junk in the member.

Nested list decode is recursion-sensitive. The visitor must use fresh loop-variable names from `context.names` at each depth and must not accidentally reuse variable names in a way that shadows active locals.

Map decode is order-sensitive. The current key-first/value-second extraction order must be preserved **exactly**. The `r.ReadMapValueBitfield(count)` call must not be skipped; it keeps the cursor in sync. Any proposal to decode key/value pairs in one pass must first prove it is byte/behavior identical for the existing map layout.

The bridge remains load-bearing after GIR-4. Do not delete `FieldKind`, do not rewrite RBA, do not migrate schema/IPC, Arrow-view (`SetFromScalars_`), or TS as part of this item, and do not remove the IR-to-`FieldMapping` projection.

The IR must stay language-neutral. Any need for `ReadInt32`, `std::string`, generated class names, or Arrow type strings is satisfied by backend lookup tables (`CppScalarInfo` via `LookupScalar`, `CppClassName`), never by fields added to IR nodes.

GIR-4 must not foreclose BIND-2's descriptor-driven codec. The bespoke edge decoder remains; descriptor-driven ABI codec work may be added later from the same IR/schema facts.

Enum and dictionary modeling are unchanged. Enum identity remains first-class in the IR while C++ decode may continue to store/read the physical `int32_t`. Dictionary remains a scalar modifier, not a structural map/list/container node.

## Files-to-touch

```text
protoc/include/cpp_backend_decode_visitor.hpp
protoc/src/cpp_backend_decode_visitor.cpp
```

New IR-driven edge decode visitor and `DecodeContext`.

```text
protoc/include/cpp_backend_type_table.hpp
protoc/src/cpp_backend_type_table.cpp
```

Verify or add support for positional read method derivation equivalent to `PositionalReadCall` (generator.cpp L1538-1555). The backend must distinguish `ReadTimestamp`, `ReadDuration`, and `ReadBinary` vs `ReadString` based on scalar logical type and Arrow type expression. Method names remain C++ backend strings, not IR fields.

```text
protoc/src/generator.cpp
protoc/include/generator_internal.hpp
```

Route generated edge decode / `DecodeRow` emission through the IR visitor. The entry point is the generated constructor `Foo(PositionalReader&)` body, which calls the new `EdgeDecodeVisitor` to emit field decoding instead of calling `EmitFieldDecode`. Keep encode on the GIR-3 IR path. Keep schema/IPC, view, TS, and RBA on the bridge.

```text
protoc/include/type_mapper.hpp
protoc/src/type_mapper.cpp
```

No new classifier. Touch only if decode needs existing IR facts exposed more directly. `MapField()` remains a thin wrapper over `BuildFieldIr()` plus projection for unmigrated emitters.

```text
protoc/tests/test_ir.cpp
protoc/tests/CMakeLists.txt
```

Add focused decode visitor assertions for:

- Nullable scalar emits `IsNull` check correctly.
- Non-nullable scalar reads value correctly (no null check).
- Binary/string ownership is copied, not borrowed (verify `emplace(reinterpret_cast<...>)` or `std::string(...)` in output).
- Nullable struct emits null check, then `ReadStruct`, then `emplace` correctly.
- Flatten-wrapped list emits outer null gate before `ReadListHeader`.
- Map decode reads key pass, then `ReadMapValueBitfield`, then value pass (bitfield MUST NOT be skipped).
- Nested list recursion uses fresh loop vars at each depth and resize+indexed assignment.
- Backend lookup is used (no hardcoded C++ strings).
- Unsupported nodes emit diagnostic or boundary, not silent decode.

Do not duplicate parity coverage in `test_ir.cpp`. Value equality across real generated fixtures belongs in the coverage harness.

```text
integration-tests/protoc-coverage/tests/test_coverage_harness.cpp
```

Extend `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` to round-trip all active fixtures and committed golden bytes through the generated edge constructor. This is the GIR-4 forcing oracle.

```text
protoc/src/recordbatch_accessor_emitter.cpp
protoc/src/recordbatch_accessor_emitter.hpp
```

Do not rewrite. Touch only if a signature adjustment is required to keep consuming the projected `FieldMapping`.

```text
protoc/src/schema_emitter.cpp
protoc/src/view_emitter.cpp
protoc/src/ts_emitter.cpp
```

Do not migrate in GIR-4. These remain bridge consumers until GIR-5, GIR-6, and GIR-7 respectively. `SetFromScalars_` stays on the bridge until GIR-6.

## Step-2 re-review (2026-07-10)

**Verdict: NEEDS-REWORK** (2 blocking, 1 should-fix). The prior four blockers are
substantially resolved; two of them relapse in secondary representative shapes.

Resolved from the prior review:
- B1 (real API): the named fictional calls (`GetStruct`/`GetList`/`Keys()`/
  `Values()`/`FLETCHER_RETURN_NOT_OK`) are all gone. Primary scalar/struct/list/map
  shapes now match `EmitFieldDecode` (generator.cpp L1599-1773).
- B2 (map): `r.ReadMapValueBitfield(count)` is present and flagged critical;
  vector-of-pairs built via `emplace_back(std::move(keys_[mi_]), …)`; key-first/
  value-second order and dup-key behaviour preserved.
- B3 (forcing test): now `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs`,
  in Files-to-touch; the Codec/View parity oracle is correctly relegated to a
  non-regression check and named as the GIR-6 `SetFromScalars_` surface.
- B4 (scope): narrowed to `EmitFieldDecode` only; `SetFromScalars_`/Arrow view left
  on the bridge (GIR-6); schema/IPC/TS/RBA explicitly out.
- Should-fixes: ownership section correctly says the edge decoder COPIES into owned
  members; FixedSizeList marked unsupported/deferred (not falsely "preserve");
  `test_ir.cpp` uses prose assertions (no hardcoded wrong expected strings).
- Locked-decision regression check: no wire change (#2), no C++ string in IR (#1),
  RBA read-only (#3), view emitter not pulled forward (#4/#8). None regressed.

Required changes:

1. **[BLOCKER — B1 relapse] Fictional `.emplace()` on non-optional locals in two
   shapes.** Map struct-value (L279-283) shows `Leaf value; … value.emplace(sr);`
   and the flatten-wrapped list (L204-205) shows `elem.emplace(er)` plus the
   undefined readers `lh_reader`/`elem_reader`. `.emplace()` exists only on
   `std::optional` members (the nullable-struct case is fine); a plain generated
   message local has no such method. The real emitter constructs inline:
   map struct-value is `by_id_.emplace_back(std::move(keys_[mi_]), Leaf(sr))`
   (generator.cpp L1749-1753); list-of-struct reads from `r` and does
   `<n>.emplace_back(sr)` (L1669-1678, which has **no** nullable branch). Rewrite
   both snippets to the real inline-construction form and fix `lh_reader`→`r`.

2. **[BLOCKER] Nested-list shape misrepresents the current emitter.** L211-229 shows
   `clear()/reserve()/push_back()` with a scalar (`int32_t`) innermost. The real
   NESTED_LIST path (generator.cpp L1681-1724) uses `<ref>.resize(lh_<d>.count)` with
   indexed assignment `<ref>[i_<d>] = <nc>(sr)`, loop vars `lh_<d>`/`i_<d>`, and
   supports **struct innermost only** — there is no scalar-innermost NESTED_LIST
   emission today. Presenting a shape the generator does not emit undermines the
   byte/behaviour-identity contract. Correct the example (resize + indexed assign,
   struct innermost) or state explicitly it is a not-yet-emitted coverage shape.

3. **[SHOULD-FIX / verify] `CppScalarInfo.positional_read`.** L107/L399 assert this
   field "already supplies" read-method names. In generator.cpp the method is derived
   at call time by `PositionalReadCall` (L1538-1555) from `storage_type` +
   `arrow_type_expr`, including the special cases `ReadTimestamp`/`ReadDuration`
   (via `arrow_type_expr.find(...)`) and `ReadBinary` vs `ReadString`
   (`arrow_type_expr == "arrow::binary()"`). Confirm the type table actually carries
   an equivalent field capturing those distinctions, or note the migration must
   add/derive it in `cpp_backend_type_table.*` — otherwise timestamp/duration/binary
   decode regresses.

## Step-2 final re-review (2026-07-10)

**Verdict: APPROVE.** All three residual items from the prior re-review are now
resolved against `protoc/src/generator.cpp` `EmitFieldDecode`:

1. **[B1 relapse — resolved]** Map struct-value (L260-275) is inline
   `by_id_.emplace_back(std::move(keys_[mi_]), Leaf(sr))` with `auto sr =
   r.ReadStruct(...)`, matching generator.cpp L1749-1753. The list-of-struct shape
   (L196-207) reads from `r` and uses `<name>->emplace_back(sr)`. No `.emplace(sr)`
   on a bare/non-optional local; no `lh_reader`/`elem_reader`. `address_.emplace(sr)`
   (L156/L164) is `std::optional::emplace` on a nullable member — correct.
2. **[B2 — resolved]** NESTED_LIST (L209-224) now uses `resize()` + indexed assign
   `<ref>[i_<d>] = <class>(sr)` with `lh_<d>`/`i_<d>` loop vars, struct-innermost,
   matching generator.cpp L1681-1724. The push_back/scalar-innermost shape is gone.
3. **[Should-fix — resolved]** The read-method name is now described as DERIVED
   (logic equivalent to `PositionalReadCall`, generator.cpp L1538-1555, keyed on
   `storage_type` + `arrow_type_expr`) and, in Files-to-touch, as a backend
   derivation to verify-or-add in `cpp_backend_type_table.*` — no false claim of a
   pre-existing stored `CppScalarInfo.positional_read`.

Locked-decision regression check (`GIR-locked-decisions.md`): no wire change (#2),
IR stays language-neutral with backend lookup (#1), RBA read-only via projection
(#3), emitter-by-emitter with view/schema/TS/RBA left on the bridge (#4), bespoke
edge path kept and descriptor-driven codec not foreclosed (#8). None regressed.

Non-blocking accuracy note (does not gate approval): the "nullable/optional
flatten-wrapped list" example (L196-207) shows an `if (!r.IsNull(5))` null gate on a
single-level list of structs, but the REPEATED_SCALAR/REPEATED_STRUCT paths
(generator.cpp L1650-1678) have **no** nullable branch — only SCALAR/STRUCT/
NESTED_LIST gate on the null bit. Implementers must reproduce EmitFieldDecode
exactly (L226 governs) and must NOT add a null gate to a genuine single-level list;
if the model can actually produce a nullable single-level list, that is a
pre-existing generator gap to surface, not a shape to synthesise here.
