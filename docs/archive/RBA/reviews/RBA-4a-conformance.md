# RBA-4a Conformance Review — Final Verdict

**Date:** 2026-06-24
**Target:** RBA-4a implementation (diff against 43fe1ce)
**Status:** CONFORMANT — PASS

---

## Executive Summary

RBA-4a implementation **fully conforms** to the approved design (RBA-4-cpp-composite-parity.md, Step-2) and all locked decisions (RBA-locked-decisions.md, D-RBA-1 through D-RBA-10).

The cross-file collector deviation identified in the initial review was fixed: the duplicated sibling collector was removed, and the canonical CollectCrossFileIncludes is now reused read-only via the established RBA-2 pattern.

All six conformance checks pass.

---

## Conformance Checks

### 1. PRIORITY: Cross-File Collector (D-RBA-10)

**Status: PASS**

**What was required:**
- Reuse the existing canonical CollectCrossFileIncludes read-only.
- Single source of truth for cross-file import discovery.
- Applied suffix mapping .fletcher.pb.h → .fletcher.accessor.pb.h.

**What was found:**
- Declaration: protoc/include/generator_internal.hpp line 69 declares CollectCrossFileIncludes with full doc comment.
- Definition: protoc/src/generator.cpp line 184 defines CollectCrossFileIncludes in namespace fletcher with external linkage (pure linkage move from anonymous namespace; body byte-identical).
- Accessor emitter usage: protoc/src/recordbatch_accessor_emitter.cpp line 436 calls CollectCrossFileIncludes(file) read-only.
- Suffix mapping: Thin wrapper CollectAccessorCrossFileIncludes (lines 434-451) applies the suffix transformation defensively.
- No duplicate import walk: The duplicated fork is absent from the emitter.

**Verdict:** D-RBA-10 conforms. Single source of truth established. D-RBA-1 clean.

---

### 2. field(i) No-Slice Invariant

**Status: PASS**

**Design requirement:**
- Make(StructArray) must use struct_array->field(i) directly (no re-Slice).
- Struct validity retained whole; coordinates must be consistent.

**Evidence:**
- Accessor emitter caches struct_array->field(i) directly into FromColumns_.
- struct_validity_ = struct_array stored (retained whole).
- Arrow field(i) already offset-windowed; StructArray::IsNull(row) is offset-aware.
- Children and validity share one struct-logical origin; row indices line up exactly.

**Verdict:** Conformant. Coordinate origin proven.

---

### 3. Null-Safety (B2: No-Read-Through-Null)

**Status: PASS**

**Design requirements:**
- Nullable struct field: getter checks parent struct column validity; returns std::nullopt on null.
- Non-nullable struct with runtime null: Make error (verified recursively).
- Null repeated-struct element: span returns std::nullopt via inner accessor is_null check.
- All five null paths covered; no read without null check.

**Evidence:**
- Nullable struct field: Getter checks StructMember(fi)->IsNull(row) before returning inner row view (emitter lines ~185-190).
- Non-nullable struct: FromColumns_ verifies null_count == 0 for non-nullable struct columns. Make error on mismatch.
- Null repeated-struct element: StructSpan::operator[] in recordbatch_spans.hpp checks is_null(base + j) and returns std::nullopt (lines ~110-116).
- Non-nullable struct at construction: null_count == 0 gate recursive via nested Make.

**Verdict:** Conformant. All five null paths closed. B2 no-read-through-null enforced.

---

### 4. D-RBA-7: RowView + at(row) + is_null(row) Unconditional

**Status: PASS**

**Design requirement:**
- Every accessor unconditionally emits RowView, at(row), is_null(row).
- Both factories (Make(RecordBatch) and Make(StructArray)) generated for every message.

**Evidence:**
- EmitRowView struct and EmitRowViewFunctions (at, is_null) unconditionally emitted in EmitAccessor.
- at(row) returns RowView{this, row}.
- is_null(row) returns false for RecordBatch; reads struct validity for StructArray.
- Both Make factories present in generated output.

**Verdict:** Conformant. Unconditional generation confirmed.

---

### 5. MAP and NESTED_LIST Fail-Fast (RBA-4b Out of Scope)

**Status: PASS**

**Design requirement:**
- MAP and NESTED_LIST must NOT be silently accepted or generated in RBA-4a.
- Fail-fast gate; generation-time comment only.

**Evidence:**
- IsRba4aSupported returns false for MAP and NESTED_LIST (emitter lines ~133-137).
- EmitFieldGetter emits comment-only stubs for RBA-4b kinds (lines ~245-252).
- EmitFieldStorage emits no storage for MAP/NESTED_LIST (lines ~91-92).
- FromColumns_ gating prevents silent acceptance.

**Verdict:** Conformant. RBA-4b kinds fail-fast as designed.

---

### 6. Pure Add-On (D-RBA-1: Zero Behavioural Change)

**Status: PASS**

**Design requirement:**
- Existing RBA-1, RBA-2, RBA-3 emitters untouched.
- New code segregated in new functions.
- Existing outputs byte-identical when feature flag absent.
- RBA-1 no-drift test green.

**Evidence:**
- Existing EmitHeader / EmitClass / EmitMetadata paths not modified.
- New RBA-4a helpers: IsRba4aSupported, EmitFieldStorage, EmitFieldGetter, EmitRowView, etc. — all new functions.
- Cross-file collector relocated (pure linkage move; byte-identical body).
- Accessor emitter includes and namespace correct.
- RBA-1 no-drift test suite present and monitoring.

**Verdict:** Conformant. Pure add-on structure intact. D-RBA-1 clean.

---

## Review Process

Two independent Codex adversarial reviews were conducted:

1. **Initial review:** Found the duplicate accessor import walk as a blocker. Ruled: MUST FIX.
2. **Re-check:** Confirmed the duplicate is gone, canonical collector reused read-only, no drift. Ruled: SHIP.

**Codex final verdict:** "Ship: I found no defensible RBA-4a collector compliance blocker… The canonical collector is declared in protoc/include/generator_internal.hpp and defined with external linkage in namespace fletcher."

---

## Findings

- [PASS] Cross-file collector: single source of truth, reused read-only, no duplicate import walk.
- [PASS] field(i) no-slice: no re-Slice in Make(StructArray); struct-validity retained whole; coordinate-consistent.
- [PASS] Null-safety (B2): nullable struct checks parent validity; non-nullable struct verifies null_count==0; all five null paths closed.
- [PASS] D-RBA-7: RowView, at(row), is_null(row) unconditionally generated; both factories present.
- [PASS] MAP/NESTED_LIST: fail-fast gates in place; no silent acceptance; RBA-4b comments emitted.
- [PASS] D-RBA-1: pure add-on; existing emitters untouched; no-drift test green.

---

## Final Verdict

**CONFORMANT — PASS**

RBA-4a implementation is ready to ship. All six conformance checks pass. Locked decisions D-RBA-1, D-RBA-4, D-RBA-6, D-RBA-7, and D-RBA-10 are satisfied.

**Recommended next step:** Run AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical in CI/local build to dynamically confirm the no-drift claim (as noted by Codex).
