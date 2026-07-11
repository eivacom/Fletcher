# GIR-2 Design: Phase 3b Parity Oracles

## Summary

GIR-2 pins the generated edge wire contract before the IR rewrite starts. It adds a parity-oracle test in `integration-tests/protoc-coverage/` that compares the two independent encode paths byte-for-byte against source-controlled golden bytes, and verifies decode by value using full field equality.

The forcing test is:

```text
ParityOracle.EncodeEqualsEncodeRowAndRoundTrips
```

The oracle covers every active message shape in `coverage.proto` that GIR-1 wires into generation. It deliberately excludes `coverage_future.proto`, which is committed but unwired because it contains scalar-leaf flatten and scalar nested-list cases that are not yet faithfully generated. Those parked cases return in GIR-10 and must then join the same oracle.

The hard invariant is unchanged: no wire-format change for any input the current flat generator already supports. If GIR-2 exposes `Encode() != EncodeRow()` or decode data loss for an active supported fixture, that is a finding and a stop-and-ask before GIR-3. Do not paper over it by weakening the oracle or moving supported fields to `coverage_future.proto`.

## Design

### Test home and target

Add a new test translation unit:

```text
integration-tests/protoc-coverage/tests/test_parity_oracle.cpp
```

Wire it into `integration-tests/protoc-coverage/CMakeLists.txt` as a new executable and discovered CTest group:

```text
coverage_parity_oracle_tests
ParityOracle.EncodeEqualsEncodeRowAndRoundTrips
```

The target depends on `generate_coverage_outputs`, includes `${GENERATED_DIR}`, and links the same runtime libraries as `coverage_harness_tests`:

```text
GTest::gtest_main
arrow::arrow
fletcher-arrow-bridge::fletcher-arrow-bridge
fletcher-pubsub::fletcher-pubsub
```

Add `GENERATED_DIR_PATH=\"${GENERATED_DIR}\"` only if the test opens committed/generated files by path. The preferred golden input path is source-controlled and passed as `PARITY_GOLDEN_DIR_PATH`.

### Fixture scope

The oracle reuses and extends `tests/coverage_fixture.hpp`. GIR-1 already centralizes the broad `CompositeCoverage` row in `MakeComposite()`, including:

```text
set and unset nullable fields
empty and non-empty containers
string and bytes values
WKT wrapper set and unset cases
timestamp and duration
package and nested enum values
nested structs
optional structs set and unset
repeated scalar and struct fields
map<string, int32>
map<string, Leaf>
message-level flatten
field-level flatten
struct-leaf nested lists at depth 2 and depth 3
```

GIR-2 should add fixture builders for every active top-level message in `coverage.proto`, not only `CompositeCoverage`:

```text
MakeNestedEnums()
MakeLeaf()
MakeBranch()
MakeFlattenedPoint()
MakeFieldFlattenedPosition()
MakeServiceRequest()
MakeServiceReply()
```

`ScalarCoverage` requires two variants with full optional/wrapper coverage:

```text
MakeScalars()
  base fixture: optional_int32 set, wrapped_int32 set, others unset

MakeScalarsAllSet()
  all optional and wrapper fields SET: optional_bool, optional_string, optional_bytes,
  optional_int32, wrapped_bool, wrapped_int64, wrapped_uint32, wrapped_uint64,
  wrapped_float, wrapped_double, wrapped_string, wrapped_bytes — exercise both
  set and unset states across the entire nullable axis
```

`CompositeCoverage` requires fixture adjustments:

```text
MakeComposite()
  base fixture with diverse content

MakeCompositeWithAlternateNullsAndEmpties()
  variant as originally defined: flips important null/container axes

MakeCompositeWithMapsNonSorted()
  NEW: adds map fixtures with ≥2-entry, non-sorted insertion order, so a reorder
  changes the golden and is caught:
    map_scalar: { "z": …, "a": …, "m": … } (not pre-sorted)
    map_struct: two or more non-alphabetical keys with struct values
  These must be added to coverage.proto as new fields if the message is immutable,
  or reuse existing map fields if the generated API allows distinct insertion orders.
```

The active generated `.ipc` messages currently guarded by CMake are:

```text
Branch
CompositeCoverage
FieldFlattenedPosition
FlattenedPoint
Leaf
NestedEnums
ScalarCoverage
ServiceReply
ServiceRequest
```

Each of these should have at least one parity case. `StructListWrapper` and `NestedStructListWrapper` are inlined flatten wrappers and do not emit standalone IPC schema streams today; they are still exercised through `CompositeCoverage`.

### `coverage_future.proto` boundary

`coverage_future.proto` remains unwired for GIR-2. Its current contents are not oracle-gated because they are known not to be faithful across generated surfaces:

```text
ScalarListWrapper
NestedScalarListWrapper
CompositeCoverageFuture.flattened_scalar_list
CompositeCoverageFuture.nested_scalar_lists
CompositeCoverageFuture.depth3_scalar_lists
CompositeCoverageFuture.optional_flattened_scalar_list
```

These are scalar-leaf flatten and scalar nested-list cases. Existing notes record current generator failures: TS syntax errors for scalar flatten wrappers, silent collapse of `List<List<int32>>` to a flat list in some outputs, and undefined generated symbols for depth-3 scalar-leaf nesting.

That is a real fixture limitation, not a wire-format exception. GIR-2 must state that these cases are parked until GIR-10, and GIR-10 must move them back into active coverage only when the generator can emit faithful, compilable output. Once active, they join the full byte-parity oracle.

### Encode byte-identity oracle

For each active message case, the test invokes the generated edge encoder and the Arrow runtime codec encoder independently, and compares both against source-controlled golden bytes. This is the bidirectional wire anchor that prevents coordinated encode drift.

**Golden byte storage and lifecycle:**

Golden files live under source control in:

```text
integration-tests/protoc-coverage/golden/
  coverage.ScalarCoverage.v1.bin
  coverage.ScalarCoverage.all-set.v1.bin
  coverage.CompositeCoverage.v1.bin
  coverage.CompositeCoverage.alternate-null-empty.v1.bin
  coverage.CompositeCoverage.maps-non-sorted.v1.bin
  coverage.Branch.v1.bin
  coverage.Leaf.v1.bin
  coverage.NestedEnums.v1.bin
  coverage.FlattenedPoint.v1.bin
  coverage.FieldFlattenedPosition.v1.bin
  coverage.ServiceRequest.v1.bin
  coverage.ServiceReply.v1.bin
```

The `.v1` suffix is the wire-contract baseline, not a schema-evolution mechanism. A new suffix is allowed only when the input was previously unsupported or when reviewers explicitly approve a new fixture. A byte change for a previously supported input is stop-and-ask per locked decision #2.

Golden regeneration is explicit and reviewed. Do not regenerate goldens automatically during normal CTest. Provide one of these controlled mechanisms:

```text
integration-tests/protoc-coverage/cmake/regenerate_parity_goldens.cmake
```

or a small checked-in helper executable/test mode:

```text
coverage_parity_oracle_tests --gtest_filter=ParityOracle.RegenerateGoldens
```

The regeneration path writes bytes produced by the current approved baseline, and any diff to `golden/*.bin` must be reviewed as wire-contract churn. Golden churn is expected for newly added fixtures or newly supported formerly parked inputs; it is not acceptable for active supported fixtures without an explicit stop-and-ask decision.

**The parity assertion:**

For each fixture (including the new map and nullable variants), the oracle must assert byte-identity at three points:

```cpp
template <typename Row, typename SchemaFn>
void ExpectEncodeMatchesGoldenAndEncodeRow(
    const Row& row,
    SchemaFn schema_fn,
    const std::filesystem::path& golden_path) {
  // Load committed golden bytes (the historical wire anchor).
  const auto golden_bytes = ReadFileBytes(golden_path);
  ASSERT_FALSE(golden_bytes.empty());

  // Assert: row.Encode() == golden bytes (generated path pinned to history).
  const fletcher::EncodedRow generated_bytes = row.Encode();
  EXPECT_EQ(std::vector<uint8_t>(generated_bytes.begin(), generated_bytes.end()),
            golden_bytes);

  // Assert: Codec.EncodeRow(ToArrowRow(row)) == golden bytes (runtime codec path
  // pinned to history). This catches coordinated encode drift where both paths
  // reorder (e.g., map entries) identically but the wire bytes change.
  auto schema = ImportNano(schema_fn());
  ASSERT_NE(schema, nullptr);

  fletcher::Codec codec(std::move(schema));
  const fletcher::ArrowRow arrow_row = ToArrowRow(row);
  const fletcher::EncodedRow codec_bytes = codec.EncodeRow(arrow_row);
  EXPECT_EQ(std::vector<uint8_t>(codec_bytes.begin(), codec_bytes.end()),
            golden_bytes);

  // Assert: both paths produce the same bytes (required by GIR-3..7 invariants).
  EXPECT_EQ(std::vector<uint8_t>(generated_bytes.begin(), generated_bytes.end()),
            std::vector<uint8_t>(codec_bytes.begin(), codec_bytes.end()));
}
```

The two encode paths are intentionally different:

```text
generated path:
  row.Encode()

runtime codec path:
  ToArrowRow(row)
  ImportNano(<Message>Schema())
  Codec(schema).EncodeRow(arrow_row)

golden anchor:
  committed source-controlled bytes for each fixture
```

This pins generated edge `Encode()` against `Codec::EncodeRow()` using the schema emitted for the same message, AND against the historical golden baseline. GIR-3..7 may rewrite generated emitters, but they must keep this oracle green.

### Decode round-trip oracle

The decode oracle has two parts.

**First, encode-decode value equality:**

For each fixture, encode it, decode the result using the runtime codec, and assert full field equality through generated views:

```text
row.Encode()
Codec(schema).DecodeRow(encoded_bytes)
<Message>View(decoded_arrow_row)
ExpectEquals(row, view)
```

This validates the runtime decoder against generated bytes. It must read back all fields in the active fixture, not just representative fields.

IMPORTANT: The `EncodedRow` must be bound to a named local variable that outlives the decode and view construction. Do not decode a temporary. The reason: `Codec::DecodeRow` may return an `ArrowRow` whose scalars borrow the source buffer via zero-copy (FixedSizeBinary decode wraps the input pointer in `std::make_shared<arrow::Buffer>(...)`). GIR-1's harness keeps the `EncodedRow` alive via a `static` for this reason. The parity oracle must follow the same discipline:

```cpp
template <typename Row, typename View, typename Expected, typename SchemaFn, typename EqualFn>
void ExpectRoundTripEquals(const Row& row,
                           SchemaFn schema_fn,
                           EqualFn equal_fn) {
  // Bind the encoded bytes to a named local (must outlive the view).
  const fletcher::EncodedRow encoded_bytes = row.Encode();

  auto schema = ImportNano(schema_fn());
  ASSERT_NE(schema, nullptr);

  fletcher::Codec codec(std::move(schema));
  // DecodeRow may borrow the buffer; encoded_bytes stays alive.
  fletcher::ArrowRow decoded = codec.DecodeRow(encoded_bytes.data(), encoded_bytes.size());

  View view(std::move(decoded));
  equal_fn(row, view);
}
```

**Second, known-golden-byte decode:**

Decode golden bytes and assert value equality:

```text
Read golden bytes from integration-tests/protoc-coverage/golden/
Codec(schema).DecodeRow(golden_bytes)
<Message>View(decoded_arrow_row)
Equals(expected_fixture_row, view)
```

This prevents encode paths from drifting together and also anchors the decode contract to the historical golden. Use the same lifetime discipline:

```cpp
template <typename View, typename Expected, typename SchemaFn, typename EqualFn>
void ExpectGoldenDecodesTo(const std::filesystem::path& path,
                           const Expected& expected,
                           SchemaFn schema_fn,
                           EqualFn equal_fn) {
  const auto golden_bytes = ReadFileBytes(path);
  ASSERT_FALSE(golden_bytes.empty());

  auto schema = ImportNano(schema_fn());
  ASSERT_NE(schema, nullptr);

  fletcher::Codec codec(std::move(schema));
  fletcher::ArrowRow decoded = codec.DecodeRow(golden_bytes.data(), golden_bytes.size());

  View view(std::move(decoded));
  equal_fn(expected, view);
}
```

### Equality definition

Use field-by-field generated/view equality, not Arrow scalar object equality as the primary oracle.

Arrow-level equality is useful for diagnostics, but it can hide or normalize behavior that the generated API must preserve. GIR-2 needs to prove that decoded values are visible through the generated Arrow view exactly as users would read them.

Define helpers in `test_parity_oracle.cpp` or a new local test helper header if shared later:

```text
ExpectEquals(const gen::ScalarCoverage& expected, const gen::ScalarCoverageView& actual)
ExpectEquals(const gen::Leaf& expected, const gen::LeafView& actual)
ExpectEquals(const gen::Branch& expected, const gen::BranchView& actual)
ExpectEquals(const gen::CompositeCoverage& expected, const gen::CompositeCoverageView& actual)
...
```

These helpers should compare every active field:

```text
all scalar primitive fields
all optional primitive fields, including unset state
all WKT wrappers, including unset state
timestamp_value and duration_value
package-scope and nested enum numeric values
strings and bytes with exact length and contents
nested struct inner fields
optional message present/absent state
repeated scalar values and order
repeated string/bytes values and order
repeated struct values and struct inner fields
map sizes
map keys (regardless of order if the API does not guarantee ordering)
map scalar values
map struct values and struct inner fields
flattened struct-list elements
optional flattened struct-list contents
depth-2 struct-leaf nested-list sizes and inner fields
depth-3 struct-leaf nested-list sizes and inner fields
message-level flatten fields
field-level flatten fields
service request payload fields
service reply fields
```

Map equality should be key-based, not position-based, unless the generated API contract explicitly guarantees map entry ordering. For `map<string, int32>`, compare the set of keys and each value. For `map<string, Leaf>`, compare the set of keys and each `Leaf` value field-by-field. This directly closes the current gap where tests check map size and only a small sample.

Floating point equality should use exact expectations appropriate to existing values (`EXPECT_FLOAT_EQ`, `EXPECT_DOUBLE_EQ`) and preserve special-value bit comparisons for GIR-10 boundary tests. GIR-2 does not need NaN/-0.0 boundary coverage unless those values are already in `coverage.proto`; GIR-10 owns the broader codec edge suite.

### Shared test helpers

The helpers `ImportNano`, `ReadFileBytes`, and `ToArrowRow` currently live in the harness TU's anonymous namespace. For the parity oracle TU to avoid use-after-free and to reuse them, promote them to a shared header or duplicate them locally in `test_parity_oracle.cpp`. If promoted to a header, declare them in:

```text
integration-tests/protoc-coverage/tests/coverage_test_helpers.hpp
```

Include signatures:

```cpp
std::shared_ptr<arrow::Schema> ImportNano(const std::string& nano_schema_json);
std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path);
fletcher::ArrowRow ToArrowRow(const gen::<Message>& row);
```

The parity oracle must have access to all three to ensure the encode and decode paths can be exercised without memory-safety violations.

### Coverage completeness for GIR-3..7

GIR-2 is the standing guard for the migration items:

```text
GIR-3 edge encode on IR:
  Encode()==EncodeRow() == golden must remain green.

GIR-4 edge decode on IR:
  encode -> decode -> Equals and golden decode must remain green.

GIR-5 schema + IPC visitor:
  GIR-2 still imports generated schemas for every active message; GIR-5 also has
  its own schema/IPC byte oracle.

GIR-6 Arrow view + ToArrowRow:
  EncodeRow path uses ToArrowRow(row), and decode equality reads through generated views.

GIR-7 TS interface + descriptor:
  GIR-2 does not validate TS descriptor bytes, but it prevents TS-related fixture edits
  from weakening the C++ wire oracle.
```

Because `coverage.proto` already spans scalars, WKT, enums, structs, maps (now with entry-order pinning), flatten, and struct-leaf nested lists, each GIR-3..7 migration must keep the broad active fixture green. `coverage_future.proto` is explicitly not part of the gate until the parked scalar-leaf cases are made compilable and faithful.

### Red-first sequence and finding handling

The intended red-first sequence is:

```text
1. Add coverage_parity_oracle_tests with TEST(ParityOracle, EncodeEqualsEncodeRowAndRoundTrips).
2. Reuse MakeComposite() and add the missing fixture builders (MakeLeaf, MakeBranch, etc.)
3. Add the new map and nullable fixture variants with correct non-sorted/all-set data.
4. Add Encode()==EncodeRow()==golden assertions first.
5. Add encode -> Codec.DecodeRow -> View -> Equals assertions.
6. Add committed golden bytes and decode-of-golden assertions.
7. Only then mark GIR-2 green.
```

For GIR-2 itself, a red result can reveal a real existing bug. Examples that must be treated as findings:

```text
generated Encode() and Codec::EncodeRow() produce different bytes for ScalarCoverage or CompositeCoverage
an unset optional decodes as set, or a set optional decodes as unset
an empty container decodes as null or as a missing field when the generated API distinguishes it
map keys decode but values are wrong or missing
map values decode but keys are wrong or reordered in a way that changes key-based lookup
Leaf fields inside map_struct, repeated_struct, Branch, or nested lists lose data
field-level flatten decodes x/y incorrectly
struct-leaf nested-list depth 2 or 3 loses an inner list level
golden bytes for an active supported fixture stop decoding to the expected value
Encode() != EncodeRow() or Encode() != golden_bytes for a supported input
```

**Per locked decision #6:** If the oracle surfaces a wire-CORRUPTING GEN defect (a bug in the current flat generator that causes `Encode()` to emit bytes that differ from the runtime codec or from the historical contract), the fix lands WITH the GIR-2 baseline. The golden `.bin` files must capture the CORRECT wire bytes (i.e., the generator is fixed first, then the golden is baselined against the fixed output). This ensures the oracle is never pinned to buggy bytes and GIR-3..7 inherit the corrected contract. A genuine existing `Encode()!=EncodeRow()` or decode-loss bug is still a stop-and-ask to surface and confirm before proceeding, but the resolution is a generator fix + golden rebaseline in the same item, not a deferred patch.

Per locked decision #9, GIR-3..7 are migration/refactor items and do not need their own red feature tests for existing behavior. They must keep this GIR-2 oracle green. If a migration changes wire bytes or value equality for an active supported fixture, the migration is red.

### Config and suite wiring

GIR-1 already added `integration-tests/protoc-coverage/` and its scoped commands. GIR-2 extends the same integration test, so the inner-loop command becomes:

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher\integration-tests\protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "CoverageHarness|ParityOracle"
```

Linux equivalent:

```bash
cd /workspaces/Fletcher/integration-tests/protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R "CoverageHarness|ParityOracle"
```

The full-suite addition is the existing protoc-coverage integration job with the new parity target included by default. No separate CI workflow is required unless the existing GIR-1 workflow hard-codes test names. If it does, add `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` to the selected CTest regex.

## Forcing-test mapping

`ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` starts red for the right reason when GIR-2 work begins because no parity oracle exists yet. The test becomes meaningful as soon as `test_parity_oracle.cpp` is wired and generation succeeds.

The forcing test should fail red before implementation in one of these acceptable ways:

```text
the new TU and CTest target exist, but parity assertions are not yet satisfied
the generated Encode() path differs byte-for-byte from Codec::EncodeRow()
the generated Encode() or Codec::EncodeRow() differs from committed golden bytes
the generated bytes decode but value equality exposes lost data
the golden bytes do not decode to the expected fixture values
```

## Risks & Unknowns

The biggest known fixture gap is scalar-leaf nested-list and scalar flatten coverage. It is already parked in `coverage_future.proto` because current generated output is not faithful across all surfaces. GIR-2 must not falsely gate those parked cases, but it also must not let active supported fields escape the oracle.

Golden-byte management is intentionally strict. A helper may regenerate files, but normal test runs must never rewrite committed goldens. A diff under `integration-tests/protoc-coverage/golden/*.bin` requires review. For previously supported active inputs, a wire-byte change is a stop-and-ask, not a routine rebaseline.

The exact generated APIs for optional flattened wrappers may not preserve every semantic distinction a proto reader would expect. The equality helper should assert the current supported generated semantics and document any limitation locally. It must not weaken broader optional/null checks for ordinary scalar, WKT, message, map, or list fields.

Map entry ordering is now explicitly pinned by the new non-sorted fixture variants, so a migration that changes the insertion-order semantics will be caught by `Encode()==golden` and the per-fixture parity assertion.

The nullable set/unset axis is now covered bidirectionally by the new `MakeScalarsAllSet()` fixture, which exercises every optional and wrapper in the **set** state. The union of base and all-set variants ensures both axes are gated.

Decode lifetime safety is now explicit: the parity oracle must bind `EncodedRow` to a named local that outlives the view. Helpers (`ImportNano`, `ReadFileBytes`) must be shared or duplicated to avoid use-after-free in the new TU.

RBA remains read-only. GIR-2 may reuse the coverage fixture that also feeds the accessor test, but it must not modify `recordbatch_accessor_emitter.cpp`, reshape RBA output, or introduce a dependency on RBA internals.

The oracle must not encode single-language type strings or create a shadow C++ type table. It may call generated C++ APIs because this is a C++ integration test, but all fixture intent should be expressed as values and field names, not as IR or Arrow type-string assumptions.

GIR-2 must not foreclose BIND-2's descriptor-driven codec path. The test should compare the current generated edge bytes to the current runtime codec bytes and golden bytes. It should not assert that the only future implementation strategy is bespoke generated C++.

## Files-to-touch

```text
integration-tests/protoc-coverage/CMakeLists.txt
integration-tests/protoc-coverage/tests/coverage_fixture.hpp
integration-tests/protoc-coverage/tests/test_parity_oracle.cpp
integration-tests/protoc-coverage/tests/coverage_test_helpers.hpp
integration-tests/protoc-coverage/golden/coverage.ScalarCoverage.v1.bin
integration-tests/protoc-coverage/golden/coverage.ScalarCoverage.all-set.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.alternate-null-empty.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.maps-non-sorted.v1.bin
integration-tests/protoc-coverage/golden/coverage.Branch.v1.bin
integration-tests/protoc-coverage/golden/coverage.Leaf.v1.bin
integration-tests/protoc-coverage/golden/coverage.NestedEnums.v1.bin
integration-tests/protoc-coverage/golden/coverage.FlattenedPoint.v1.bin
integration-tests/protoc-coverage/golden/coverage.FieldFlattenedPosition.v1.bin
integration-tests/protoc-coverage/golden/coverage.ServiceRequest.v1.bin
integration-tests/protoc-coverage/golden/coverage.ServiceReply.v1.bin
```

Optional helper files if the implementation chooses an explicit regeneration script instead of a gated test mode:

```text
integration-tests/protoc-coverage/cmake/regenerate_parity_goldens.cmake
integration-tests/protoc-coverage/README.md
```

Proto additions (new fixtures needed for map entry-order pinning):

```text
coverage.proto — new map fields or variant message if MakeCompositeWithMapsNonSorted requires them
```

No production generator files are required for GIR-2 unless the forcing test exposes a genuine existing wire-parity or decode-loss bug. If that happens, flag it as a finding and stop for direction before changing the generator or rebaselining golden bytes. Per locked decision #6, any wire-corrupting GEN defect that surfaces is fixed with the GIR-2 baseline (the golden captures corrected bytes).

## Step-2 re-review (2026-07-10)

**Verdict: APPROVE.** All three prior blockers and both should-fixes are
resolved. One minor, non-blocking tightening noted below.

Prior findings, confirmed:

1. **Blocker 1 (encode golden anchors) — RESOLVED.**
   `ExpectEncodeMatchesGoldenAndEncodeRow` now asserts three points:
   `row.Encode()==golden_bytes`, `Codec.EncodeRow(ToArrowRow(row))==golden_bytes`,
   and `generated_bytes==codec_bytes`. Both independent encode paths are pinned to
   the source-controlled golden, so a coordinated encode drift (both paths reorder
   identically) now changes bytes vs. golden and is caught. This is the exact hole
   the prior review flagged.
2. **Blocker 2 (map entry-order) — RESOLVED.** `MakeCompositeWithMapsNonSorted()`
   adds a 3-entry non-sorted scalar map (`{"z","a","m"}`) and a ≥2-entry
   non-alphabetical struct map, each with its own committed golden
   (`coverage.CompositeCoverage.maps-non-sorted.v1.bin`). The base fixture's
   single-entry `map_struct` could not catch struct-map reorder; the variant closes
   that. Because both maps are ≥2 entries and golden-anchored, any change to wire
   emission order is caught regardless of whether the generated container preserves
   insertion order.
3. **Blocker 3 (nullable both states) — RESOLVED (see note).**
   `MakeScalarsAllSet()` exercises the SET state across the optional/wrapper axis to
   complement the base fixture's mostly-unset state, with its own golden.
4. **Should-fix 4 (decode-lifetime) — RESOLVED.** The borrow contract is stated
   explicitly (zero-copy FixedSizeBinary buffer borrow, GIR-1's keep-alive
   precedent): `EncodedRow`/golden bytes bound to a named local that outlives the
   `View`, no decoding of temporaries, and `ImportNano`/`ReadFileBytes`/`ToArrowRow`
   promoted to a shared header (or duplicated) so the new TU is memory-safe. Both
   decode helper templates follow the discipline.
5. **Should-fix 5 (locked #6) — RESOLVED.** It is now explicit that a
   wire-corrupting GEN defect is fixed first and the golden is baselined against the
   corrected output, landing WITH the GIR-2 baseline (not deferred), and that a
   genuine `Encode()!=EncodeRow()` / decode-loss finding is stop-and-ask to surface
   before proceeding. Matches locked #6 and #2.

No regression against SPEC §Phase 3b (the design strengthens, not weakens, the
`Encode()==EncodeRow()` + golden-decode contract) or locked #2/#6/#9. Golden
churn policy honours #2 (stop-and-ask on byte change for a supported input).

**Minor tightening (non-blocking):** `optional_int32` and `wrapped_int32` are set
by the base `MakeScalars()`, and `MakeScalarsAllSet()`'s set-list also includes
`optional_int32` (and its prose header says "all ... SET"). As written, those two
fields are only ever exercised in the SET state — their UNSET state is not covered
by either fixture. This is a small hole in "both axes for *every* field," but the
unset-decode path is a uniform per-field validity mechanism already exercised by
`optional_bool/string/bytes` and the other eight wrappers, so the *class* is
covered. Suggest either leaving `optional_int32`/`wrapped_int32` unset in the
all-set variant (they are already set in the base) or adding a one-line note that
the base fixture is their unset complement — no re-review required.
