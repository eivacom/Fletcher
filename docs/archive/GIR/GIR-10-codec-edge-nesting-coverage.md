# GIR-10 — Codec Edge/Boundary + Nesting Coverage

## Summary

GIR-10 is a coverage and enablement item, not a wire-format redesign. It adds runtime codec edge/boundary tests for Arrow type families that are currently under-tested, and it un-parks scalar-leaf flatten/nested-list fixtures that GIR-1 deliberately left in `coverage_future.proto`.

Locked decision #2 remains the controlling invariant: every previously-supported fixture in `coverage.proto` must keep byte-identical wire, IPC, TS descriptor, and accessor goldens. A byte change for an already-supported input is a STOP-AND-ASK. Newly enabled `List<List<scalar>>` / `List<List<List<scalar>>>` shapes are new coverage and get fresh goldens. Existing `CompositeCoverage` fields and wire bytes are untouched; scalar-leaf nesting fixtures live in a new top-level message `ScalarNestedCoverage` with its own schema/IPC/TS goldens (NO RBA accessor golden — the RBA/rust opts are omitted for this fixture, locked #3).

## Design

**1. Codec Edge/Boundary Coverage (§3c)**

Add a runtime codec edge suite for direct Arrow `Codec` coverage. These tests do not require proto generation unless explicitly noted; they live closest to the codec so they can exercise Arrow families that protobuf cannot express.

| Family / boundary | Test location | Fixture needed | Expected status | Bug class if RED |
|---|---|---:|---|---|
| Dense union, active scalar + active varlen child | `arrow-bridge/tests/test_codec_edge.cpp` | No | Pure regression guard; `DenseUnionRoundtrip` already covers one active int | Encode/decode union child dispatch, type-code handling, offset handling |
| Sparse union, multiple active children including string | `test_codec_edge.cpp` | No | Pure regression guard; sparse active string is already covered once | Sparse inactive child/null handling |
| Intervals: month/day/nano or Arrow-supported interval variants | `test_codec_edge.cpp` | No | Likely RED-first because not visible in current first-pass codec tests | Unsupported logical-kind path, precision/layout loss |
| Time32 / Time64 across units | `test_codec_edge.cpp` | No | Likely RED-first for unit precision or unsupported unit branches | Encode/decode temporal unit mapping, narrowing |
| Duration across units | `test_codec_edge.cpp` | No | Mixed: WKT duration ns covered through generator; direct Arrow duration units likely RED-first | Unit conversion/precision loss |
| Decimal128 positive, zero, negative, max precision-scale representative | `test_codec_edge.cpp` | No | Negative decimal likely RED-first; positive decimal already covered once | Sign extension, fixed-width byte order, precision/scale handling |
| Large string / large binary | `test_codec_edge.cpp` | No | Likely RED-first if codec only handles 32-bit length binary/string | Length prefix path, scalar class dispatch |
| String/binary view types if Arrow build exposes them | `test_codec_edge.cpp`, compile-gated on Arrow availability | No | Likely RED-first/possibly unsupported | Unsupported Arrow type ID, zero-copy buffer lifetime |
| Fixed-size binary | `test_codec_edge.cpp` | No | Pure regression guard if Arrow validates width; earlier plan says encode over-read non-reachable | Width validation, decode length fidelity |
| `INT*_MIN`, `INT*_MAX`, `UINT*_MAX` for all integer widths | `test_codec_edge.cpp` | No | Mostly regression guard; current integer test covers some max values but not all mins | Signed/unsigned read/write, narrowing |
| Float/double NaN, `+Inf`, `-Inf`, `-0.0` | `test_codec_edge.cpp` | No | Likely RED-first if equality uses semantic `Equals` only | Bit preservation, especially NaN payload and negative zero |
| Empty string, embedded NULs, multi-byte UTF-8 | `test_codec_edge.cpp` | No | Regression guard for binary-safe length prefix; embedded NUL should be explicit | Varlen length/string-view truncation |
| Empty binary + embedded zero bytes | `test_codec_edge.cpp` | No | Regression guard | Varlen length/payload handling |
| Empty list/map, all-null list elements, high-index nulls | `test_codec_edge.cpp` | No | Regression guard; all-null list and some wide bitfields already covered | Null-bitfield count/read bounds |
| Wide null-bitfields: ≥9 top-level fields and ≥9 nullable list/map values | `test_codec_edge.cpp` | No | Regression guard plus high-index variant likely useful | Bitfield byte count, high-bit indexing |
| Encode→decode→encode determinism for all edge cases | helper assertion in `test_codec_edge.cpp` | No | Regression guard | Non-canonical decode or dropped payload |

**Concrete decode error paths (distinct from GIR-11 fuzz):**
- Truncated buffer: list header incomplete, struct field incomplete, null-bitfield incomplete
- Bad union type-code: discriminant points to non-existent union child
- List/map count overflow: value > `INT32_MAX`
- Fixed-size-binary width mismatch: decoded bytes ≠ fixed width

For floats, do not rely only on Arrow scalar `Equals`. Decode and compare raw bit patterns for `NaN`, infinities, and `-0.0` using `std::bit_cast`/`memcpy` into `uint32_t`/`uint64_t`.

For currently unsupported runtime codec families, there is no expected wire-byte change because they have no existing supported wire baseline. If a new test demonstrates that a type was already accepted but incorrectly encoded, classify it using the bug protocol below.

**2. List<List<scalar>> Enablement (§3d)**

The IR grammar already has the right shape: `IrNode` supports recursive `LIST` with a nested `element`, and `BuildFlattenedRepeated()` (ir.cpp:274-326) already constructs scalar-leaf nested `List<List<...Scalar>>` for `repeated ScalarListWrapper` / `repeated NestedScalarListWrapper`. The IR is correct; the emitter surfaces are not faithful.

Verified IR build facts:
- Lines 301-308: depth-0 scalar leaf wraps in one LIST (→ `List<Scalar>`); depth-0 becomes `List<Scalar>`
- Lines 319-326: depth>0 scalar leaf wraps in depth+1 LIST levels (→ `List<List<...<Scalar>>>`)

Current emitter state and required GIR-10 changes:

| Surface | Current scalar-leaf nested-list readiness | Required GIR-10 change |
|---|---|---|
| IR structure | Can represent `List<Scalar>`, `List<List<Scalar>>`, `List<List<List<Scalar>>>` | No grammar change; IR unit coverage for scalar-leaf at depth 2/3 already provided by fixture activation (see §2.2 below) |
| C++ edge storage declaration | `FieldKind::NESTED_LIST` (generator.cpp:317-322) projects through `fi.mapping.nested_class` — a struct type. Scalar leaf would need IR-driven storage bypass or FieldKind extension to a scalar-leaf variant | Extend FieldKind OR emit IR-native storage via new helper (no FieldKind projection). **Block RBA generation for this message** — see §2.3 |
| C++ edge decode | `EmitNestedList()` (cpp_backend_decode_visitor.cpp:179) explicitly rejects non-STRUCT leaf with `EmitUnsupported` | Make nested-list decode recursive, dispatching on leaf kind (scalar or struct) |
| C++ edge encode | Existing traversal is already recursive leaf-agnostic (cpp_backend_type_table.cpp `EmitList` calls `EmitValue(*list.element)` → `EmitScalar` at leaf) | No change needed |
| Arrow view getter | `ClassifyNestedList()` (cpp_backend_view_visitor.cpp:54) does unconditional `std::get<ir::StructNode>` — would throw `bad_variant_access` on scalar leaf | Add scalar-leaf variant path or refactor to generic recursive leaf handling |
| `ToArrowRow` | Nested list builder assumes `coord_type` is a struct and calls `ToArrowRow(v)` (cpp_backend_view_visitor.cpp:422/446) | Make nested-list Arrow scalar construction recursive and leaf-type driven (scalar → `arrow::scalar`, struct → nested `arrow::struct_` + recurse) |
| Schema/IPC | `IsSchemaRepresentable()` (cpp_backend_schema_visitor.cpp:44-48) explicitly drops nested lists whose leaf is not `STRUCT` | Treat scalar-leaf nested lists as representable and emit nested Arrow `LIST` children down to scalar |
| TS interface | Recursive `InterfaceType()` (ts_backend_visitor.cpp:135-145) resolves scalar leaf via `TsLookupScalar(...).ts_type_text` → `number[]` / `number[][]` / `number[][][]` | No change needed — verify TS correctly emits recursive nested-array types and no `wireType: ,` / empty `[]` tuple syntax errors; add TS descriptor golden |
| RBA accessor | Read-only per locked decision #3; assumes struct-leaf nested list | **Exclude the new scalar-leaf-nesting fixture from RBA generation** — land in a separate message with RBA-omitting generation unit (see §2.3) |

**2.1. Fixture strategy: New top-level message**

Create a new top-level message `ScalarNestedCoverage` (not in `CompositeCoverage` — no field 11/13/15/17 insertion). Land these shapes:

| Field | Shape | Handling |
|---|---|---|
| `ScalarListWrapper flattened_scalar_list = 1` | `List<int32>` via singular flatten wrapper | Scalar-leaf flatten; TS syntax fix verification |
| `optional ScalarListWrapper optional_flattened_scalar_list = 3` | nullable `List<int32>` | Nullable scalar-leaf flatten |
| `repeated ScalarListWrapper nested_scalar_lists = 5` | `List<List<int32>>` | Primary forcing scalar-leaf nesting shape |
| `repeated NestedScalarListWrapper depth3_scalar_lists = 7` | `List<List<List<int32>>>` | Depth-3 scalar recursion (optional: may be left parked if depth-3 recursion is deferred) |

The new `ScalarNestedCoverage` message gets its own `.bin`, `.ipc`, `.ts`, and generated C++/view/schema goldens. Existing `CompositeCoverage`, `ServiceRequest`, `ServiceReply`, and all other fixtures generate byte-identical goldens.

**2.2. Coverage assertions**

The test `Nesting.ListOfListOfScalarRoundTrips` (or `Nesting.ScalarNestedListRoundTrips` if clearer) proves faithfulness:

1. Generated edge encode → generated edge decode → field equality passes for all `ScalarNestedCoverage` fields
2. Runtime codec `EncodeRow(ToArrowRow(row))` → `DecodeRow` → generated view equality passes
3. The Arrow schema field for `nested_scalar_lists` is exactly `list<list<int32>>`, not `list<int32>`
4. The generated C++ API exposes nested containers (`std::vector<std::vector<int32_t>>` for field 5)
5. The generated TS interface says `number[][]` for field 5 and `number[][][]` for field 7 (if depth-3 is enabled), not `number[]` or `[]`
6. **Existing fixtures remain byte-identical:** `ScalarCoverage`, `CompositeCoverage`, `Branch`, `Leaf`, `FlattenedPoint`, `FieldFlattenedPosition`, `ServiceRequest`, `ServiceReply` generate unchanged `.bin`, `.ipc`, `.ts`, C++ edge/view/schema, and RBA outputs

**2.3. RBA safety (locked decision #3)**

The new `ScalarNestedCoverage` message must NOT be fed to the RBA accessor emitter. RBA is read-only and assumes struct-leaf nested lists; scalar-leaf nested-list accessors would crash/miscompile.

**Strategy: Create a dedicated generation unit that omits RBA.**

In `integration-tests/protoc-coverage/CMakeLists.txt` (or equivalent generation orchestration), create a separate protoc invocation for `ScalarNestedCoverage` that sets:
- `--fletcher_opt=ipc,ts` — the real token set for this fixture. (The C++ edge header and the Arrow view/`ToArrowRow` are emitted UNCONDITIONALLY, so there are no `edge`/`view` tokens; the valid tokens are `schema_only|ts|ipc|accessor|rust`, generator.cpp ~1726-1735.)
- **OMIT** `--fletcher_opt=accessor` (RBA C++ accessor disabled for this fixture — RBA is read-only and assumes struct-leaf; locked #3)
- **OMIT** `--fletcher_opt=rust` (Rust RBA disabled for this fixture)

Confirm that the existing coverage target (which generates RBA for all messages) continues to produce unchanged RBA outputs for `CompositeCoverage` etc.; the new unit produces no `.fletcher.accessor.pb.h` or `.fletcher.rs` for `ScalarNestedCoverage`. State explicitly in the design: "RBA retains its struct-leaf-only scope until RIR. `ScalarNestedCoverage` is generated without RBA."

**3. Bug-Handling Protocol**

Every RED `CodecEdge.*` failure must be classified before fixing:

(a) Genuine existing runtime codec bug. Examples: signed overflow, unchecked length, NaN payload loss, `-0.0` canonicalization, decimal sign corruption, null-bitfield indexing error, decode accepting malformed input. Record the affected path: encode, decode, builder construction, null-bitfield, list/map count, union dispatch, temporal unit conversion, or scalar value extraction. If the fix changes wire bytes for a previously-supported input, STOP-AND-ASK before landing. If the failing type was previously rejected or unsupported, fix-by-construction and add fresh tests/goldens.

(b) Missing feature or unfaithful parked shape. Examples: scalar-leaf nested list collapsed to flat vector, TS `wireType` omitted, undefined wrapper class, schema drops nested scalar list. Implement/enable the feature and add new goldens only for the newly supported shapes.

Guarding strategy: the three-gate sequence stays intact.

```text
GIR-1 harness -> GIR-2 oracles -> Phase 2 rewrite / GIR-10 coverage
```

Do not rebaseline a golden to bless a pre-existing codec bug. Fix the bug, verify the RED test now passes, then regenerate the oracle so the baseline captures correct bytes. Any wire-affecting fix for a previously-working input must land with the guarded GIR-2 baseline and requires STOP-AND-ASK if bytes differ.

**4. Schema-Evolution Negative Tests (§3d family)**

Per spec §3d, add explicit tests that schema-evolution changes are detected, not silently mis-decoded:

- v1 buffer (no `nested_scalar_lists` field, earlier wire layout) decoded under a v2 schema (with `nested_scalar_lists`) must be rejected or produce null/empty, not uninitialized data
- v2 buffer with `nested_scalar_lists` decoded under v1 schema (pre-field) must either reject or skip the field per proto spec, not corrupt adjacent fields

Add these as `Nesting.SchemaEvolutionNegative*` cases in the nesting test suite. They are distinct from GIR-11 fuzz (§3e) and GIR-10-owned.

**5. Scope Boundary**

IN:

- Runtime codec edge/boundary tests in `arrow-bridge/tests`.
- GIR-1/GIR-2 coverage harness fixture updates.
- IR-driven emitter faithfulness for scalar-leaf nested lists.
- New `ScalarNestedCoverage` top-level message with fresh goldens.
- RBA-omitting generation unit for scalar-leaf nesting fixtures.
- Schema-evolution negative tests.

OUT:

- GIR-11 property/fuzz testing.
- RBA rewrite or RBA migration to IR, per locked decision #3.
- C++ string literals in IR, per locked decision #1.
- Wire-format redesign.
- Broad generator refactors unrelated to scalar-leaf nesting.

**6. Test Wiring + Inner Loop**

Put the codec edge suite in a new TU:

```text
arrow-bridge/tests/test_codec_edge.cpp
```

Keep `test_codec.cpp` as the existing broad smoke/regression file; `test_codec_edge.cpp` should contain focused `CodecEdge.*` cases and helpers for bit-exact scalar comparison.

Put scalar-leaf nesting coverage in the protoc coverage integration layer:

```text
integration-tests/protoc-coverage/proto/coverage.proto
integration-tests/protoc-coverage/proto/coverage_future.proto (ScalarListWrapper, NestedScalarListWrapper if needed)
integration-tests/protoc-coverage/proto/coverage_scalar_nested.proto (new, defines ScalarNestedCoverage)
integration-tests/protoc-coverage/tests/coverage_fixture.hpp (add ScalarNestedCoverage fixture population)
integration-tests/protoc-coverage/tests/test_coverage_harness.cpp
integration-tests/protoc-coverage/tests/test_parity_oracle.cpp
integration-tests/protoc-coverage/tests/test_view_visitor.cpp
integration-tests/protoc-coverage/tests/test_ts_visitor.cpp
integration-tests/protoc-coverage/tests/test_nesting.cpp (new, or extend existing if present)
```

Add dedicated test names `Nesting.ListOfListOfScalarRoundTrips` and `Nesting.SchemaEvolutionNegative*`. They use the same generated outputs and golden directories as the coverage harness.

Existing goldens stay byte-identical by rerunning the current parity set and comparing unchanged files. New scalar-leaf shapes create fresh goldens only after the generated C++/view/schema/TS surfaces prove the shape is faithful.

Inner loop for GIR-10:

```text
ctest -R "CodecEdge|Nesting|CoverageHarness|ParityOracle|ViewVisitor|TsVisitor"
```

Also run the broader protoc coverage target before handoff because scalar-leaf nesting touches multiple emitters:

```text
ctest --test-dir integration-tests/protoc-coverage/build
ctest --test-dir arrow-bridge/build
```

## Forcing-test mapping

`CodecEdge.*` covers §3c. Each test is a direct runtime codec assertion, with encode→decode equality and encode→decode→encode determinism where applicable. Float tests use bit comparison. Malformed or unsupported cases assert clear throws rather than crashes or silent corruption.

`Nesting.ListOfListOfScalarRoundTrips` covers §3d nesting. It fails for the GIR-1 parked reasons until all scalar-leaf nested surfaces are faithful: no TS syntax error, no silent C++ collapse, no undefined wrapper symbols, no schema drop, and no view/ToArrowRow struct-leaf assumption.

`Nesting.SchemaEvolutionNegative*` covers §3d schema-evolution. Each test asserts that schema changes (added/removed fields, reordered fields) are detected, not silently mis-decoded.

Existing GIR-2 `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` remains the byte-identity gate for previously supported fixtures. The GIR-10 nesting tests add new golden anchors for scalar-leaf nested fixtures only.

## Risks & Unknowns

The largest known risk is the **§3d storage blocker**: C++ edge storage still depends on `FieldKind::NESTED_LIST` carrying a struct `nested_class`. A scalar-leaf nested list must either:
- Extend FieldKind to a scalar-leaf variant (but this feeds the read-only RBA emitter and is locked #3 BLOCKED), or
- Bypass FieldKind with IR-native storage generation (preferred, and the design commits to this in §2.3).

Confirm the chosen path is implemented and RBA is excluded from the new `ScalarNestedCoverage` generation unit.

The schema visitor currently marks nested scalar-leaf lists as unrepresentable (cpp_backend_schema_visitor.cpp:44-48), so schema/IPC will silently omit the shape until fixed. This must be treated as an unfaithful feature gap, not a golden update.

The view helper layer may not have a reusable nested scalar-list view type today. Adding one should be narrow and should not disturb existing `ArrowNestedList<StructView>` / `ArrowNestedList2<StructView>` behavior.

Some Arrow families in `CodecEdge.*` may not be available in every pinned Arrow version, especially view types. Gate those tests by compile-time availability or omit view-specific cases if the dependency cannot express them.

RBA remains read-only. The new scalar-leaf-nested fixtures are excluded from RBA generation via a dedicated generation unit (§2.3). Confirm no widening of existing fixtures.

## Files-to-touch

```text
arrow-bridge/tests/test_codec_edge.cpp (new)
arrow-bridge/tests/CMakeLists.txt

integration-tests/protoc-coverage/proto/coverage.proto
integration-tests/protoc-coverage/proto/coverage_scalar_nested.proto (new, or extend coverage_future.proto)
integration-tests/protoc-coverage/tests/coverage_fixture.hpp
integration-tests/protoc-coverage/tests/test_coverage_harness.cpp
integration-tests/protoc-coverage/tests/test_parity_oracle.cpp
integration-tests/protoc-coverage/tests/test_view_visitor.cpp
integration-tests/protoc-coverage/tests/test_ts_visitor.cpp
integration-tests/protoc-coverage/tests/test_nesting.cpp (new or extended)
integration-tests/protoc-coverage/CMakeLists.txt

protoc/src/ir.cpp (no change to grammar; used for coverage verification only)
protoc/src/type_mapper.cpp (FieldKind projection review/no change if IR-native storage bypass chosen)
protoc/src/generator.cpp (IR-native storage declaration for scalar-leaf nested lists)
protoc/src/cpp_backend_decode_visitor.cpp (make EmitNestedList recursive over scalar/struct leaves)
protoc/src/cpp_backend_view_visitor.cpp (add scalar-leaf path to ClassifyNestedList and ToArrowRow recursion)
protoc/src/cpp_backend_schema_visitor.cpp (treat scalar-leaf nested lists as representable; emit nested Arrow LIST)
protoc/src/ts_backend_visitor.cpp (verify recursive InterfaceType/ElementDescriptor already works; add TS golden if needed)

protoc/include/generator.hpp
protoc/include/cpp_backend_decode_visitor.hpp
protoc/include/cpp_backend_view_visitor.hpp
protoc/include/cpp_backend_schema_visitor.hpp
protoc/include/ts_backend_visitor.hpp

integration-tests/protoc-coverage/golden/*
```

## Step-2 review (2026-07-11)

See the previous rework prompt. This fresh rewrite incorporates:

1. **[locked #2 — golden byte-identity; no widening]** New top-level `ScalarNestedCoverage` message with fresh goldens; `CompositeCoverage` untouched and gaps 11/13/15/17 preserved. Locked-#2 byte-identity for existing fixtures is guaranteed.

2. **[§3d emitter misattribution → corrected]** The design table now correctly identifies blocking sites: storage (FieldKind projection in type_mapper/generator), decode (EmitNestedList explicit STRUCT check), view (ClassifyNestedList bad_variant_access), schema (IsSchemaRepresentable scalar-leaf drop), TS (already works — verify). Encode is already recursive.

3. **[locked #3 — RBA read-only]** RBA-omitting generation unit for `ScalarNestedCoverage` (no `accessor` / `rust` opts). Confirm byte-identical RBA outputs for existing fixtures.

4. **[spec §3d family — schema-evolution tests added]** `Nesting.SchemaEvolutionNegative*` tests (v1 decoded as v2, vice versa) added as explicit GIR-10 coverage. Distinct from GIR-11 fuzz.

5. **[concrete decode error paths added]** Truncated buffer, bad union type-code, list/map count overflow, fixed-size-binary width mismatch enumerated in §3c.

## Step-2 re-review (2026-07-11)

**Verdict: NEEDS-REWORK — 1 blocking, 1 minor.** The substance is sound and
verified against real code: locked #2 (byte-identity) and locked #3 (RBA
read-only) are honoured by the operative strategy, SPEC §3c/§3d are matched, and
the four previously-flagged emitter sites are now correctly attributed. One
load-bearing internal inconsistency in the Summary blocks approval.

Verified against code (not just the doc's assertions):
- **Storage blocker real** — `generator.cpp` `StorageDecl` `NESTED_LIST`
  (317-322) wraps `fi.mapping.nested_class` (a struct class name); no
  scalar-leaf path.
- **Decode blocker real** — `EmitNestedList` (cpp_backend_decode_visitor.cpp:179)
  rejects non-STRUCT leaf via `EmitUnsupported`.
- **View blocker real** — `ClassifyNestedList` (cpp_backend_view_visitor.cpp:54)
  does unconditional `std::get<ir::StructNode>` (→ `bad_variant_access` on scalar
  leaf); `ToArrowRow`'s `EmitNestedList` (416+) hard-assumes `coord_type =
  arrow::struct_(...)` and calls `ToArrowRow(v)`.
- **Schema blocker real** — `IsSchemaRepresentable` (cpp_backend_schema_visitor.cpp:44-48)
  returns representable only when the innermost leaf is `STRUCT`.
- **Encode already leaf-agnostic** — `cpp_backend_type_table.cpp`
  `EmitList`→`EmitValue`→`EmitScalar`. "No change" is correct.
- **TS already recursive** — `InterfaceType` (135-145) yields `number[][]`;
  `ElementDescriptor`/`AppendComposite` recurse. "Verify-only" is correct.
- **Item 1 honoured** — fields 11/13/15/17 remain parked in
  `coverage_future.proto` `CompositeCoverageFuture`; `CompositeCoverage`
  untouched; new top-level `ScalarNestedCoverage` carries the shapes.
- **Item 4 present and distinct** — `Nesting.SchemaEvolutionNegative*` (v1↔v2)
  is separated from §3e fuzz.

**Required changes (numbered):**

1. **[BLOCKER — Summary contradicts §2.3 / locked #3]** Summary (line 7) still
   says `ScalarNestedCoverage` gets "schema/IPC/TS/**accessor** goldens." That
   directly contradicts §2.3, §2.1, §2.2 (item 6), and §Risks, all of which OMIT
   the `accessor`/`rust` opts so NO RBA accessor is generated for this scalar-leaf
   message. Taken literally, the controlling-invariant Summary promises an RBA
   accessor golden for a scalar-leaf-nested message — exactly what locked #3
   forbids (the read-only RBA emitter assumes struct-leaf and would
   crash/miscompile). Delete the word "accessor" from line 7 so it reads
   "...with its own schema/IPC/TS (no RBA accessor) goldens." NOTE: this is a
   stale-wording defect, NOT a proposed locked-#3 deviation — the operative
   strategy in §2.3 correctly omits RBA — so it is a fix, not a STOP-AND-ASK.

2. **[MINOR — non-existent opt tokens]** §2.3 (lines 97-98) prescribes
   `--fletcher_opt=edge` and `--fletcher_opt=view`. Those tokens do not exist:
   the generator recognizes only `schema_only|ts|ipc|accessor|rust`
   (generator.cpp:1726-1735; corroborated by robustness-plan §Phase-2 no-drift
   token list). The C++ edge header is emitted unconditionally and the Arrow view
   header whenever `schema_only` is unset, so the correct RBA-omitting opt string
   is simply `--fletcher_opt=ipc,ts`. Functionally the unknown tokens are
   harmless no-ops (so RBA-omission safety is not at risk), but correct the doc to
   `ipc,ts` and drop the fictional `edge`/`view` bullets to avoid misleading the
   implementer.

Non-blocking note: the new `coverage_scalar_nested` stem needs its own
stem-scoped `.ipc` validate-guard invocation mirroring coverage/enum_coverage;
`CMakeLists.txt` is already in Files-to-touch.

§3c family enumeration, the classify-before-fix + STOP-AND-ASK-on-wire protocol,
and the genuinely red-first `Nesting.*` mapping are all intact.
