# RBA-7 Conformance Review

**Date:** 2026-06-24  
**Reviewer:** Claude Code (manual adversarial conformance)  
**Design doc:** plans/RBA-7-docs-capstone-parity.md  
**Locked decisions:** plans/RBA-locked-decisions.md  
**Diff base:** e1e28f2  

## Executive Summary

**VERDICT: PASS**

The RBA-7 implementation is conformant to the approved design and all locked decisions.

### Key Findings

**Capstone Mechanism (D-RBA-8):**
- Single shared proto, fixture JSON, and expected oracle JSON enforced
- Both C++ and Rust read the same committed files
- Transitive parity proven: observed_cpp == expected AND observed_rust == expected => observed_cpp == observed_rust
- Zero hardcoding of expected values; both languages parse JSON at runtime

**D-RBA-7 Spec Correction (Accurate & Properly Scoped):**
- C++ struct-child windowing corrected: StructArray::field(i) already windowed, must NOT re-Slice
- Arrow-rs columns() not pre-windowed, must .slice()
- This is documentation/specification correction only; no code changes
- PM-confirmed scope block approves this as factual-mechanism correction with intent preserved
- Intent (correct child windowing, no read-through, no pre-rebasing) preserved

**D-RBA-1 No-Drift (Additive Only):**
- No changes to existing generator output files
- No existing emitter touched
- Documentation and test-harness additions only
- RBA-1 no-drift test passes

**Documentation (Pillar 2):**
- `--fletcher_opt=accessor` and `--fletcher_opt=rust` documented in protoc/README.md
- Additive; no existing meaning changed
- README example (minimal C++ accessor usage) mirrored in test_accessor_readme_example.cpp
- Spec §7 corrected (C++ struct-child wording)
- Spec §10 "Out of scope" untouched

**Test Coverage (All Field Kinds + Nulls):**
- SCALAR (non-nullable id, nullable label)
- STRUCT (nullable maybe_child)
- REPEATED_SCALAR (samples with null element)
- REPEATED_STRUCT (children with null element)
- MAP scalar-value (scores with null value)
- MAP message-value (child_by_key with null value)
- NESTED_LIST (nested_children with null inner list)
- Metadata (schema + field, with C++ nullptr == Rust empty-map normalization)
- Explicit null-path assertions: maybeChild(1) == nullopt, label(1) == nullopt, etc.

**Tests Pass:** C++ ctest 10/10, Rust cargo 10/10

---

## Locked Decision Conformance Matrix

| Decision | Status | Evidence |
|----------|--------|----------|
| D-RBA-1 | PASS | No existing outputs changed; docs/test-only additions |
| D-RBA-2 | PASS | accessor + rust tokens documented, orthogonal to ts/ipc/schema_only |
| D-RBA-3 | PASS | Column-oriented caching verified (capstone constructs once, per-row getters index cached arrays) |
| D-RBA-4 | PASS | Positional type validation exercised (accessor construction validates, tests assert correct readout) |
| D-RBA-5 | PASS | Generic metadata read-back (schema_metadata() / field_metadata(i) exposed as opaque maps) |
| D-RBA-6 | PASS | All field kinds covered (SCALAR, STRUCT, REPEATED_SCALAR, REPEATED_STRUCT, MAP, NESTED_LIST) |
| D-RBA-7 | PASS | Spec corrected (C++ field(i) already windowed; Rust columns() must slice); intent preserved; no code change |
| D-RBA-8 | PASS | Transitive parity proven (shared proto, fixture, oracle JSON; both languages parse + compare) |
| D-RBA-9 | PASS | RecordBatch construction proven; Table out-of-scope preserved |
| D-RBA-10 | PASS | Shared proto integrated via package-keyed assembler (no changes needed this round) |

---

## Single Source of Truth Enforcement (D-RBA-8 Proof)

**Shared Proto:** `integration-tests/accessor-capstone/proto/accessor_capstone.proto`
- Both C++ and Rust generate from the same `.proto` file

**Shared Fixture:** `integration-tests/accessor-capstone/fixtures/accessor_capstone_fixture.json`
- C++ test: LoadJson() reads from CAPSTONE_FIXTURES_DIR (CMake define)
- Rust test: load_json() reads from FLETCHER_TEST_CAPSTONE_FIXTURES (env var)
- Both build in-memory Arrow batch using native builders (not IPC)

**Shared Oracle:** `integration-tests/accessor-capstone/fixtures/accessor_capstone_expected.json`
- C++ test: asserts observed_json == expected (both parsed at runtime)
- Rust test: asserts observed == expected (both parsed at runtime)
- Neither test hardcodes expected values; both read the ONE committed file

**Transitive Guarantee:**
- C++ asserts: observed_cpp == expected ✓
- Rust asserts: observed_rust == expected ✓
- Conclusion: observed_cpp == observed_rust (D-RBA-8 proven)

---

## Capstone Coverage

**Schema Metadata:** `{"capstone": "rba-7", "owner": "accessor"}`

**Field Metadata (Positional Index, Mixed Present/Absent):**
- Index 0: `{"role": "id"}`
- Index 1: `{}`
- Index 2: `{}`
- Index 3: `{"role": "samples"}`
- Indices 4-7: `{}`

**Null Cases (All Required Paths Present):**
1. Nullable scalar (label at row 1): null → no value
2. Nullable struct (maybe_child at row 1): null → nullopt
3. Null scalar element (samples row 0, element 1): null → probed via is_null(j)
4. Null struct element (children row 0, element 1): null → nullopt
5. Null map scalar value (scores row 0, element 1): null → probed via value_is_null(j)
6. Null map message value (child_by_key row 0, element 1): null → nullopt
7. Null inner list (nested_children row 0, element 1): null → nullopt

**Rows:** 3 deterministic rows (row 0 with full data + nulls, row 1 null-heavy, row 2 normal)

---

## Two Flagged Deviations (Both Acceptable)

### (a) nlohmann_json Test Dependency
**Status:** Acceptable  
**Scope:** Design Risks/Unknowns: "C++ can use repo's existing test JSON dependency if present, or add a test-only dependency"  
**Implementation:** Added to conanfile.py as test-only dependency  
**Justification:** Required for C++ test to parse shared committed JSON (not hardcode it)

### (b) NESTED_LIST via (fletcher.flatten) Wrapper Pattern
**Status:** Acceptable  
**Scope:** Design Risks/Unknowns: "If suggested pattern doesn't hit that kind, choose existing composite fixture pattern already used by RBA-4/RBA-6"  
**Implementation:** Uses repeated InnerList (existing flatten-wrapper pattern)  
**Justification:** Correctly classifies as NESTED_LIST; test asserts null inner list path

---

## No Blocking Issues

All conformance gates pass:
- Shared proto ✓
- Shared fixture ✓
- Shared oracle ✓
- Transitive parity proof ✓
- All field kinds + nulls ✓
- Metadata normalization ✓
- D-RBA-7 correction ✓
- D-RBA-1 no-drift ✓
- Docs additive ✓
- §10 untouched ✓
- Tests pass ✓

---

## Conclusion

**RBA-7 is CONFORMANT.** The capstone correctly proves C++/Rust parity from one model (D-RBA-8), the D-RBA-7 spec correction accurately reflects shipped code behavior with intent preserved, and all documentation additions are additive with zero impact on D-RBA-1. Tests pass (10/10 both languages); no breaking changes to existing outputs.

**Recommendation: Ready for merge.**
