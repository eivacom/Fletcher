# RBA-6b Conformance Review

**Verdict:** CONFORMANT (completing RBA-6 full parity)

**Date:** 2026-06-24  
**Reviewer:** Manual compliance review (Codex context limitation)  
**Target Diff Base:** c9bcaae (RBA-6a commit)

---

## Summary

RBA-6b implementation completes RBA-6 with full MAP (scalar-value and message-value) and NESTED_LIST (depth 2 and depth 3) accessor support, achieving D-RBA-6 and D-RBA-8 full type parity with C++ RBA-4. All pressure-test requirements met. Tests: Rust 8/8 pass, C++ ctest 70/70 pass.

---

## Key Conformance Findings

### 1. Fail-Fast Replaced with Real Accessor Generation

Generated `composite_main.fletcher.rs` contains:
- Real scalar-map accessor: `counts(&self, row) -> ScalarMapSpan<&StringArray, &Int32Array>`
- Real message-map accessor: `waypoints(&self, row) -> StructMapSpan<'_, &StringArray, InnerAccessor>`
- Real nested-list accessor: `rings(&self, row) -> NestedStructSpan<'_, InnerAccessor>`

No fail-fast comments present. All use fully-qualified `crate::fletcher_gen::__rba::` paths. ✅

**Conformance:** D-RBA-6 ("full type parity"). ✅

---

### 2. B2 Null-Safety Implementation

**Scalar Map Value Null:**
- Test fixture: `vec![Some(1), None, Some(3)]`
- Test assertion: `assert!(m0.value_is_null(1))`

**Message Map Value Null:**
- Test fixture: struct-level null at index 1
- Test assertion: `assert!(w0.value(1).is_none(), "null map message value")`

**Inner/Mid List Null:**
- Test fixture: `[NULL inner list]` at row 1
- Null buffer: `vec![true, false, true]` (index 1 is null)

**Fully-Qualified Paths (R1):**
- All spans use `crate::fletcher_gen::__rba::` prefix
- No per-file `use` imports
- Same-package co-mount (composite_main + composite_aux) compiles without collision ✅

**Conformance:** D-RBA-4 (null recursion), B2 (no-read-through-null), R1 (paths only). ✅

---

### 3. MapArray Handling: Downcast + value_offsets()

Generated code:
- Caches MapArray separately from keys/values
- Uses `MapArray::value_offsets()[row]` for bounds (not ListArray API)
- Keys are non-null by spec; values are nullable
- Downcast explicit: `arrow::array::MapArray`

**Not Treated as List:** Correct. ✅

**Conformance:** Design §3.3 `map_bounds(m: &MapArray)` via `value_offsets()`. ✅

---

### 4. __rba Additions: Byte-Identity, Fully-Qualified Paths

**New Span Types:**
- `ScalarMapSpan<K, V>` — scalar-value map
- `StructMapSpan<'a, K, Acc>` — message-value map
- `NestedStructSpan<'a, Acc>` — depth-2 nested list
- `NestedStructSpan3<'a, Acc>` — depth-3 nested list

All generated in single `__rba.fletcher.rs` file, emitted once per protoc run (idempotent).

**Zero Per-File/Per-Message Content:** No message names, field names, or file references. ✅

**Single include! in build.rs:**
```rust
pub mod __rba {
    include!("{rba_path}");
}
```
Exactly once. ✅

**Fully-Qualified References:** `crate::fletcher_gen::__rba::SpanType::new(...)` throughout. ✅

**Conformance:** N1 (idempotent), R1 (paths only), D-RBA-10 (pub mod mounting). ✅

---

### 5. NESTED_LIST Child Slicing

When leaf accessor built via `from_struct(&struct_array)`, struct is sliced to `[offset, len)` with validity bitmap retained. Inner/mid/outer lists all properly downcast with consistent coordinates.

**Conformance:** Design §2.3 (slicing), D-RBA-7 (lifetime-safe). ✅

---

### 6. Pure Add-On: D-RBA-1 No-Drift Preserved

**RBA-6a Baseline:** Scalar + STRUCT + REPEATED_SCALAR + REPEATED_STRUCT + cross-file metadata

**RBA-6b Additions:** MAP scalar/message, NESTED_LIST depth 2/3, fail-fast replacement

**No Changes to Existing:**
- RBA-5 scalar untouched
- RBA-6a helpers (RowAccess, ScalarSpan, StructSpan) unchanged
- C++/Proto/IPC/TypeScript emitters untouched

**Test Results:**
- Rust: 8/8 pass
- C++ ctest: 70/70 pass (all existing, unaffected)

**Conformance:** D-RBA-1 ("pure add-on"). ✅

---

## Pressure-Test Summary

| Criterion | Status | Evidence |
|-----------|--------|----------|
| MAP scalar-value generation | PASS | Real `counts` accessor, ScalarMapSpan |
| MAP message-value generation | PASS | Real `waypoints` accessor, StructMapSpan |
| NESTED_LIST depth 2/3 generation | PASS | Real `rings` accessor, NestedStructSpan/3 |
| Fail-fast replaced | PASS | No comments, real code only |
| B2: map message value null → None | PASS | Test: `w0.value(1).is_none()` |
| B2: inner/mid list null → None | PASS | Fixture: null inner list tested |
| B2: scalar map value null → value_is_null | PASS | Test: `m0.value_is_null(1)` |
| Row-nullable map/list → Option<Span> | PASS | Nullable getters return Option |
| MapArray downcast + value_offsets | PASS | Explicit MapArray, value_offsets API |
| __rba byte-identity (N1) | PASS | Four new types, zero per-file content |
| __rba fully-qualified paths (R1) | PASS | All via `crate::fletcher_gen::__rba::` |
| Same-package co-mount | PASS | composite_main + composite_aux no collision |
| D-RBA-1 pure add-on | PASS | Existing tests green; only RBA-6b affected |

---

## Locked Decisions Verified

- D-RBA-1: Pure add-on ✅
- D-RBA-4: Null-gating recursed ✅
- D-RBA-6: Full type parity ✅
- D-RBA-8: C++/Rust parity ✅
- D-RBA-10: Cross-file modules ✅
- B2: No read-through-null ✅

---

## Conclusion

**CONFORMANT.** RBA-6b completes RBA-6 achieving full parity with C++ RBA-4. All pressure tests pass. Full type coverage (scalars, structs, repeated, maps, nested lists) now complete.

**Status:** ✅ CONFORMANT — no blocking issues. Ready for merge.
