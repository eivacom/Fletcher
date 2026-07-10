# GIR-4 Code Review: Edge Decode IR Visitor

**Date:** 2026-07-10  
**Reviewer:** Claude Code (static analysis)  
**Target:** GIR-4 migration of EmitFieldDecode to recursive IR visitor  
**Status:** BLOCKING ISSUE FOUND

---

## Summary

GIR-4 successfully migrates the edge C++ DECODE path (EmitFieldDecode) to a language-neutral IR visitor, achieving byte-for-byte behavior parity with the pre-migration implementation. The new visitor correctly handles all decode shapes: nullable/non-nullable scalars, owned string/binary copies, structs, single/nested lists, and maps with proper cursor synchronization. All critical edge cases are handled correctly, and test coverage is comprehensive. However, a **blocking git diff issue** prevents the patch from being applied cleanly.

---

## Summary of Findings

### Severity Counts

- **Blocking:** 1 (untracked files not in diff)
- **Should-fix:** 0
- **Nit:** 0

### Blocking Issue

**GIR-4-Untracked-Files:** The new files cpp_backend_decode_visitor.hpp and cpp_backend_decode_visitor.cpp are not included in the git diff. CMakeLists.txt and generator.cpp reference them, but the files themselves are untracked. Applying the patch will fail at configure-time.

---

## Detailed Findings

### [BLOCKING] Git Diff Does Not Include New Files

**Location:** CMakeLists.txt L21, generator.cpp include  

**Issue:**  
The modified CMakeLists.txt adds src/cpp_backend_decode_visitor.cpp to the build target, and generator.cpp includes cpp_backend_decode_visitor.hpp. However, these two new files appear as ?? (untracked) in git status and are not included in the diff. Applying the tracked diff to the base branch will fail at configure/compile time with missing source/header.

**Required Action:**  
Stage and commit the new files:
```
git add protoc/include/cpp_backend_decode_visitor.hpp
git add protoc/src/cpp_backend_decode_visitor.cpp
```

**Impact:** Patch cannot be cleanly applied. Blocking merge/PR.

---

## Logic & Correctness Review (Non-Blocking)

### Decode Visitor Correctness: PASS

**Scalar Handling:**
- Non-nullable scalars emit direct reads without IsNull gate (lines 75-104). Correct.
- Nullable scalars properly gate on IsNull(si_) before read (lines 80-84, 90-92, 97-99). Correct.
- Owned string copies wrap in std::string() constructor (line 94). Correct.
- Owned binary copies use reinterpret_cast and std::string(p, n) constructor (lines 83-84). Correct.
- Read-method derivation delegates to CppScalarInfo::positional_read via LookupScalar() (lines 35-40). Verified by test at test_ir.cpp:547. Correct.

**Struct Handling:**
- Non-nullable struct emits ReadStruct(NestedClass::Schema()->n_children) then emplace(sr) (lines 114-117). Correct.
- Nullable struct adds IsNull() gate before same sequence (lines 109-113). Correct.

**List Handling:**
- Repeated scalar uses ReadListHeader(), clear, reserve, loop with element reads (lines 139-154). String elements use emplace_back(), scalars use push_back(). Matches old code exactly.
- Repeated struct same loop pattern, no null gate on list itself (lines 156-165, verified by test assertion at line 669). Correct.

**Nested List Handling: CRITICAL REVIEW**
- Depth tracking correctly counts list levels; validates innermost is struct (lines 173-178). Correct.
- Fresh loop variables per depth: lh_<d> and i_<d> ensure no collision (lines 225-226). List[List[Struct]] produces lh_0, i_0, lh_1, i_1 as per spec. Correct.
- Uses resize and indexed assignment, not push_back on nested lists (line 228, verified by test line 668). Correct.
- Nullable nested list: calls .emplace() to construct optional, then dereferences (*n_) in loops (lines 186-197). Correct.

**Map Handling: CRITICAL CHECK**
- Key-first pass collects keys into temporary vector (lines 231-236). String keys use emplace_back(), others push_back(). Correct.
- **ReadMapValueBitfield call is PRESENT and CORRECTLY POSITIONED** (line 238): auto vbf = r.ReadMapValueBitfield(count); This CRITICAL call appears between key pass and value pass. Omitting it would desynchronize cursor and corrupt subsequent reads. Design spec line 254 emphasizes this. CORRECT.
- Value-second pass reads values and emplace_back key-value pairs (lines 241-264). Scalar values read via Read*() method, struct values call ReadStruct() and construct nested message. Correct.
- Keys moved via std::move(keys_[mi_]) preserving duplicate behavior and iteration order. Correct.

**Unsupported Nodes:**
- Emits diagnostic comment "// unsupported field skipped: <reason>" never silent decode body (lines 273-276). Matches design spec. Correct.

### Sequential-Cursor Safety: PASS

Each node kind consumes exact number of tokens old EmitFieldDecode did. Critical: maps call ReadMapValueBitfield() between key and value passes maintaining cursor sync. Nested lists iterate through fresh ReadListHeader() calls per depth. No missing reads, no stale cursor state.

### Read-Method Derivation: PASS

CppScalarInfo::positional_read from backend table handles all scalar types. Test at test_ir.cpp:547 verifies mapping for int32. No unmapped types, no missing methods.

### Dead-Code Removal: PASS

- PositionalReadCall() function fully removed (old lines 1535-1555). No dangling declarations.
- EmitFieldDecode() function fully removed (177 lines old). No orphaned call sites.
- Call sites updated at generator.cpp:1805 and 1815 to invoke cpp_backend::EmitFieldDecodeFromIr().
- Header properly included (generator.cpp:19).

### Memory Safety: PASS

- Visitor constructor takes ostringstream& by reference, moves value_expr string, stores file descriptor pointer safely.
- String/binary copied into owned std::string, never borrowed from PositionalReader.
- No uninitialized variables, no buffer overflows, no use-after-free hazards.

### Tests: PASS

**Unit Test (test_ir.cpp EdgeDecodeVisitorEmitsPositionalReads):**
Exercises all shapes with substantive assertions on exact emitted code:
- Nullable/non-nullable scalars with IsNull() gates
- String and binary owned copies
- Nullable/non-nullable structs
- Repeated scalars and structs (no null gate on list)
- Maps: CRITICAL assertion at line 639 checks ReadMapValueBitfield presence
- Nested lists: fresh variables, no push_back, proper sizing
- Unsupported nodes: diagnostic comments

**Integration Test (test_coverage_harness.cpp):**
Added field-by-field equality oracle (ExpectEquals overloads) for every fixture. Verifies:
- All scalar types (bool, int32, int64, uint32, uint64, sint32, sint64, fixed32, fixed64, sfixed32, sfixed64, float, double, string, bytes)
- Optional primitives and WKT wrappers with presence AND value checks
- Nested structs, lists, maps recursively
- Nested list depth 2 and 3 with triple-nested loop verification
- Golden-byte round-trip: 11 fixtures reconstructed via edge constructor from both freshly-encoded and committed golden bytes
- Assert decode→encode byte-equality (pins golden to wire contract)

Tests are comprehensive and substantive.

---

## Conclusion

**GIR-4 implementation is correct from logic, safety, and test perspectives.** All decode shapes emit byte-identical code to pre-migration EmitFieldDecode. Critical ReadMapValueBitfield cursor-sync call is present and correctly positioned. Comprehensive unit and integration tests verify shape and round-trip byte-equality.

**One blocking issue:** The two new files must be committed to enable patch application.

**Verdict:** APPROVE with required fix for untracked files.

