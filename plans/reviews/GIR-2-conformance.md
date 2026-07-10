# GIR-2 Conformance Review

**Date:** 2026-07-10  
**Item:** GIR-2 (Phase 3b parity oracles) + generator fix (locked decision #6)

## Verdict: CONFORMS

### Pressure-Test 1: Generator Fix (Decision #6)
PASS - Fix is correct, minimal, complete.
- Line 1590 removes stray `*` from nullable bytes WriteBinary path
- Produces `n->data()` instead of `*n->data()` (correct dereference)
- Matches non-nullable branch at line 1603
- No other nullable/repeated/map bytes path has the antipattern
- Wire-corrupting bug fixed before golden baseline
- Golden bytes contain corrected wire contract

### Pressure-Test 2: Byte-Identity Oracle
PASS - Triple assertion per fixture.
1. row.Encode() == golden_bytes (generated edge pinned)
2. Codec.EncodeRow(ToArrowRow(row)) == golden_bytes (runtime codec pinned)
3. generated_bytes == codec_bytes (both paths agree, catches coordinated drift)

**Both-state nullable coverage:**
- MakeScalars(): optional_int32 SET, wrapped_int32 SET, others mostly unset
- MakeScalarsAllSet(): all optional/wrapper SET except optional_int32/wrapped_int32
  (already covered by base - Step-2 re-review approved tightening)

**Map entry-order pinning:**
- MakeCompositeWithMapsNonSorted() creates non-sorted maps
- map_scalar: {"z","a","m"}, map_struct: {"y","b"}
- 2+ entries each with committed golden
- Wire reorder changes bytes vs golden (caught)

**Decode oracle:** 
- Round-trip: Encode → Decode → View → field equality
- Golden decode: Read golden → Decode → View → field equality
- Lifetime discipline: EncodedRow bound to named local outliving View
- Field-by-field equality covers all active types (scalars, optional, WKT, enums, structs, repeated, maps, flatten, nested lists depth 2/3)

### Pressure-Test 3: Golden Baseline Integrity
PASS - 12 goldens gated + reviewable.
- All present: ScalarCoverage (2 variants), CompositeCoverage (3), Branch, Leaf, NestedEnums, FlattenedPoint, FieldFlattenedPosition, ServiceRequest, ServiceReply
- RegenerateGoldens test SKIPPED unless FLETCHER_REGEN_PARITY_GOLDENS=1
- Regeneration writes current Encode() (reviewed as churn)
- ctest never rewrites committed goldens
- README §Regenerating: stop-and-ask for already-supported input changes

### Pressure-Test 4: Locked-Decision Conformance
PASS - All decisions honored.
- #2: Byte identity pinned (Encode==EncodeRow==golden, decode round-trip equality)
- #6: Fix lands WITH baseline; golden captures corrected bytes
- #9: Oracle is green guard for GIR-3..7 migrations
- #3: RBA read-only; no changes to recordbatch_accessor_emitter.cpp
- No IR type strings; no BIND-2 path foreclosure

### Pressure-Test 5: Flagged Deviations
PASS - All sanctioned by Step-2 re-review.
- MakeScalarsAllSet() doesn't set optional_int32/wrapped_int32: re-review approved (lines 567-576)
- ImportNano(OwnedSchema) vs aspirational (std::string): design shows OwnedSchema signature; implementation matches exactly

## Cross-Checks

**Fixture coverage:** All 9 active messages + variants. Depth-2/3 nested lists, flatten, optional structs, repeated containers, maps all exercised.

**CMake:** New executable coverage_parity_oracle_tests, depends on generate_coverage_outputs, includes GENERATED_DIR, links Arrow/Fletcher libs, PARITY_GOLDEN_DIR_PATH set, discovered via gtest_discover_tests.

**Test helpers:** ImportNano, ReadFileBytes, ToArrowRow promoted to shared header (coverage_test_helpers.hpp). No use-after-free; ToArrowRow found via ADL.

**Architecture:** Design conformance complete. Locked decisions #2/#6/#9 directly enforced. Red-first ready. Green oracle gates migrations.

## Summary

Blocking issues: None.
Deviations: None.
Generator fix scope: Verified complete.
Locked decision adherence: Full.
Implementation quality: High.

READY FOR MERGE.
