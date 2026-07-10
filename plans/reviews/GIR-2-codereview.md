# GIR-2 Code Review: Phase 3b Parity Oracles + Generator Wire-Corruption Fix

Reviewed: 2026-07-10 (static analysis; test suite verified passing per step 3)
Scope: protoc/src/generator.cpp (wire-format bug fix), test oracle, fixtures/helpers, goldens, CMake.

## SUMMARY

GIR-2 addresses a production generator bug (nullable-bytes WriteBinary path corrupts wire format) and introduces a parity oracle—comprehensive regression suite pinning the edge wire contract before GIR-3. No blocking issues. Test design is rigorous: triple-fold byte-identity assertion, round-trip decode, coordinated-drift guard via committed goldens.

---

## 1. GENERATOR FIX (protoc/src/generator.cpp, lines 1589-1596)

### THE BUG

OLD code:
  o << "w.WriteBinary(reinterpret_cast<const uint8_t*>(" << "*" << n
    << "->data()), " << n << "->size());\n";

For optional_bytes, emits: w.WriteBinary(reinterpret_cast<const uint8_t*>(*optional_bytes_->data()), ...)

Problem:
- optional_bytes_->data() returns const char* (buffer pointer)
- *optional_bytes_->data() dereferences it → char (single byte value, e.g., 0x41)
- reinterpret_cast<const uint8_t*>(char) interprets the BYTE VALUE as a MEMORY ADDRESS → garbage pointer
- Writes corrupted bytes for all SET optional/wrapped bytes fields

### THE FIX

NEW code:
  o << "w.WriteBinary(reinterpret_cast<const uint8_t*>(" << n
    << "->data()), " << n << "->size());\n";

Emits: w.WriteBinary(reinterpret_cast<const uint8_t*>(optional_bytes_->data()), ...)

✓ Correct: optional_bytes_->data() is const char*, safely recast to const uint8_t*.
✓ Aligns with non-nullable bytes path (line 1606: val.data()).

### ADJACENT PATHS AUDIT

Repeated bytes (line 1635, EmitScalarWrite):
- n + "[li_]" is std::string → .data() emitted correctly (no leading *).

Map keys/values (lines 1689, 1700, EmitScalarWrite):
- "k" and "v" are actual values → .data() correct.

Non-nullable bytes (line 1604–1607):
- val from value_or() is std::string → val.data() correct.

✓ BUG ISOLATED TO NULLABLE BYTES PATH; ALL ADJACENT PATHS ALREADY CORRECT.

---

## 2. ORACLE TEST CORRECTNESS

### TRIPLE-FOLD ASSERTION

1. row.Encode()                      == golden
2. Codec.EncodeRow(ToArrowRow(row)) == golden
3. row.Encode()                      == Codec.EncodeRow(ToArrowRow(row))

Rationale: (1) & (2) pin wire contract; (3) is the GIR-2 invariant. Catches coordinated reorder bugs.

✓ Byte vectors compared exactly; not vacuous.

### ROUND-TRIP & DECODE

- Encode → Codec.DecodeRow → View → ExpectEquals (field-by-field)
- Load golden → Decode → View → ExpectEquals

✓ Zero-copy borrow discipline honored (buffers outlive views).

### FIELD-BY-FIELD EQUALITY RIGOR

ScalarCoverage (88–125): 24 assertions covering:
- Non-nullable scalars: bool, int32–uint64, sint32/sint64, fixed32/fixed64, sfixed32/sfixed64, float, double, string, BYTES
- Optional: bool, int32, string, BYTES
- WKT wrappers: bool, int32–uint64, float, double, string, BYTES
- Enums, temporal fields

CompositeCoverage (140–257): scalars, optional scalars, nested/optional structs, repeated (scalar/string/BYTES/struct), maps (by key + inner fields), nested lists (depth 2–3), flattened.

✓ Not tautological; comprehensive across all types and container variants.

### DECODE LIFETIME / BORROW CONTRACT

ExpectRoundTripEquals (293–301):
  const fletcher::EncodedRow encoded_bytes = row.Encode();  // Named local
  ...
  fletcher::ArrowRow decoded = codec.DecodeRow(encoded_bytes.data(), encoded_bytes.size());
  View view(std::move(decoded));                             // View borrows from encoded_bytes
  ExpectEquals(row, view);                                   // Use while encoded_bytes in scope

✓ Named buffer outlives view's use.

ExpectGoldenDecodesTo (304–315): Same pattern with golden_bytes local.

✓ No temporary-decode antipattern; zero-copy contracts respected.

---

## 3. FIXTURES & COVERAGE

### NULLABLE & CONTAINER AXES

MakeScalars():
- optional_int32: SET; optional_bool: UNSET
- OPTIONAL_BYTES: UNSET (exercises null path)
- wrapped_int32: SET; wrapped_bool: UNSET
- WRAPPED_BYTES: UNSET

MakeScalarsAllSet():
- optional_int32: UNSET (complement); optional_bool: SET
- OPTIONAL_BYTES: SET to "\x01\x02" ← Exercises fixed nullable-bytes path with actual data
- wrapped_int32: UNSET; wrapped_bool: SET
- WRAPPED_BYTES: SET to "\x03\x04\x05"

✓ Union covers BOTH set and unset for all nullable fields (see fixture comment).

MakeCompositeWithAlternateNullsAndEmpties():
- Uses MakeScalarsAllSet() (includes optional_bytes SET)
- repeated_bytes NON-EMPTY: {"\x09\x08", "\x07"}
- Other collections EMPTY (opposite of base)

✓ Exercises repeated bytes with actual data.

MakeCompositeWithMapsNonSorted():
Out-of-order keys catch future reordering bugs.

### CRITICAL TEST CASE

The nullable optional_bytes SET case is explicitly tested:
- MakeScalarsAllSet (line 333) includes set_optional_bytes("\x01\x02")
- Composite variants using it (line 337)
- Would fail under old generator (*optional_bytes_->data() → garbage)
- Passes under new generator

---

## 4. GOLDEN MECHANICS

### SOURCE-TREE BASELINE

CMakeLists.txt (line 199):
  PARITY_GOLDEN_DIR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/golden"

✓ Goldens in source tree (not build tree); treated as reviewed artifacts.

### GATED REGENERATION

test_parity_oracle.cpp 367–370:
  const char* flag = std::getenv("FLETCHER_REGEN_PARITY_GOLDENS");
  if (flag == nullptr || std::string(flag).empty())
      GTEST_SKIP() << "set FLETCHER_REGEN_PARITY_GOLDENS=1 to ...";

✓ Requires explicit env var; skipped in normal ctest.
✓ Won't accidentally rewrite in CI.

### FILE ROBUSTNESS

ReadFileBytes (coverage_test_helpers.hpp 44–52):
Missing file returns empty vector with gtest failure. Test assertion (line 272) asserts result is non-empty.

✓ Missing file → red failure with path name.

Golden count: 12 .bin files; 12 CheckFixture calls + 12 RegenerateGoldens writes all match.

---

## 5. CMAKE WIRING

✓ add_dependencies on generate_coverage_outputs (codegen)
✓ target_include_directories includes ${GENERATED_DIR}
✓ Links GTest::gtest_main, arrow, fletcher-arrow-bridge, fletcher-pubsub
✓ PARITY_GOLDEN_DIR_PATH uses CMAKE_CURRENT_SOURCE_DIR (source tree)
✓ gtest_discover_tests with PRE_TEST mode
✓ Golden availability: absolute path computed at cmake configure time, reliable at runtime

---

## 6. MEMORY SAFETY

✓ No use-after-free (named locals outlive borrows)
✓ No leaks (RAII throughout)
✓ No dangling refs (assertions while View in scope)
✓ Uninitialized variables: all properly initialized

---

## FINDINGS

Severity | Count | Notes
---------|-------|-------
BLOCKING | 0     | None
NON-BLOCKING | 0  | None
NIT      | 0     | None

---

## CONCLUSION

GIR-2 successfully fixes a production wire-corruption bug and establishes a rigorous parity oracle pinning the edge wire contract before GIR-3. Generator fix is correct; test design is comprehensive with triple-fold assertion, critical nullable-bytes SET case explicitly tested via MakeScalarsAllSet, golden infrastructure properly gated and source-controlled, CMake and lifetime management all sound. Ready for merge.
