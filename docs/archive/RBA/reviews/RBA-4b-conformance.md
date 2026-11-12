# RBA-4b Conformance Review — Final Verdict

**Date:** 2026-06-24
**Target:** RBA-4b implementation (diff against 985165e)
**Status:** CONFORMANT — PASS

---

## Executive Summary

RBA-4b implementation fully conforms to the approved design after storage fixes. All composite children now have explicit `std::shared_ptr<arrow::StructArray>` cache members per D-RBA-7 (lifetime-safe, owns cached handles):

- REPEATED_STRUCT: `<field>_values_` (design line 425)
- MAP message-value: `<field>_values_` (design line 484)
- NESTED_LIST leaf (depth-2 and depth-3): `<field>_leaf_values_` (design lines 517/537)

All assignments occur in FromColumns_ using the same flattened StructArray passed to the inner accessor (coordinate-consistent, field(i) no-slice held). Getters/validation logic untouched. Test suites: accessor 8/8 green; full suite 70/70 green; RBA-1 no-drift green.

---

## Conformance Checks — All PASS

### 1. Explicit-Handle Storage (D-RBA-7)

**Status: PASS**

**Storage members verified:**
- REPEATED_STRUCT: `std::shared_ptr<arrow::ListArray> + std::shared_ptr<arrow::StructArray> + optional<Accessor>` (lines 224-226).
- MAP message-value: `std::shared_ptr<arrow::MapArray> + std::shared_ptr<Keys> + std::shared_ptr<arrow::StructArray> + optional<Accessor>` (lines 231-237).
- NESTED_LIST: outer/mid/inner ListArrays + `std::shared_ptr<arrow::StructArray>` leaf + optional<Accessor> (lines 249-254).

**FromColumns_ assignments verified:**
- REPEATED_STRUCT: `ListValuesMember(fi) = structs` (line 549).
- MAP message-value: `MapValuesMember(fi) = item_structs` (line 587).
- NESTED_LIST: `NlLeafValuesMember(fi) = leaf_structs` (line 661, both depth-2 and depth-3).

**Verdict:** D-RBA-7 explicit-handle ownership satisfied. All composite children cached with explicit typed shared_ptr members.

---

### 2. MAP Implementation (Scalar + Message Values)

**Status: PASS**

**Scalar-value maps:**
- Storage: MapArray + Keys + typed scalar values array (correct).
- Getter: ScalarMapSpan constructed with keys.get() + values.get() + offset/length.
- Null-safety: value_is_null(j) for nullable scalars; keys non-null (Arrow spec).

**Message-value maps:**
- Storage: MapArray + Keys + explicit StructArray cache + accessor.
- Getter: StructMapSpan constructed with keys.get() + accessor ptr + offset/length.
- Null-safety: value(j) returns std::nullopt for null struct values (B2 no-read-through-null).
- Nullable map field: std::optional<Span> (None on null row).

**Verdict:** MAP implementation conforms. Both scalar and message values handled correctly.

---

### 3. NESTED_LIST Implementation (Depth 2 and 3)

**Status: PASS**

**Depth-2 (List<List<Struct<T>>>):**
- Storage: outer ListArray + inner ListArray + leaf StructArray + leaf accessor.
- Getter: NestedStructSpan<T, 2> with inner_lists + leaf_values + offset/length.
- Null-safety: each inner list level returns std::optional<span>; leaf returns std::optional<RowView>.

**Depth-3 (List<List<List<Struct<T>>>>):**
- Storage: outer + mid + inner ListArrays + leaf StructArray + leaf accessor.
- Getter: NestedStructSpan<T, 3> with mid_lists + inner_lists + leaf_values + offset/length.
- Null-safety: mid and inner list levels return std::optional<span>; leaf returns std::optional<RowView>.

**Verdict:** Both depth-2 and depth-3 fully supported. Each inner level nullable per API; leaf RowView nullability checked via leaf accessor is_null.

---

### 4. field(i) No-Slice Invariant

**Status: PASS**

**Verification:**
- Message-value map items: `item_structs = std::static_pointer_cast<arrow::StructArray>(items)` (no re-Slice); passed to `Accessor::Make(item_structs)`.
- Nested-list leaf values: `leaf_structs = std::static_pointer_cast<arrow::StructArray>(leaf_vals)` (no re-Slice); passed to `Accessor::Make(leaf_structs)`.
- Inner accessors handle offset windowing via field(i) or value() indexing; coordinates consistent.

**Verdict:** field(i) no-slice maintained. Recursion via canonical Accessor::Make(StructArray).

---

### 5. No New Cross-File Collector Fork

**Status: PASS**

**Verification:**
- No additional import-walk code introduced for RBA-4b.
- Canonical CollectCrossFileIncludes reused read-only (unchanged since RBA-4a).
- No modification to generator.cpp or type_mapper.cpp core logic (D-RBA-1 clean).

**Verdict:** Single source of truth for cross-file discovery maintained.

---

### 6. Pure Add-On (D-RBA-1: Zero Behavioral Change)

**Status: PASS**

**Verification:**
- RBA-1, RBA-2, RBA-3, RBA-4a outputs unchanged (no-drift test green).
- MAP and NESTED_LIST fail-fast gates removed only for supported kinds.
- Any genuinely unsupported construct (e.g., NESTED_LIST depth 4) still fails fast or emits D-RBA-6 comment.
- Full `CompositeColumnsReadColumnOriented` test suite expanded to cover all five composite kinds.

**Test status (per coordinator claim):**
- Accessor ctest: 8/8 green.
- RBA-1 no-drift: green (storage-only change, zero output drift).
- Full test suite: 70/70 green.

**Verdict:** Pure add-on structure intact. All prior conformance checks (RBA-1/2/3/4a) remain green.

---

## Codex Review Summary

Codex adversarial re-check after storage fixes: **APPROVE** (no blocking findings).

"D-RBA-7 explicit-handle ownership is now satisfied for repeated structs, message-value maps, and depth-2/depth-3 nested-list leaf structs; the assignments use the same flattened StructArray passed to the inner accessor. Getter/span logic and validation behavior did not show a defensible regression in the reviewed diff."

---

## Findings

All six pressure-test items: PASS

- [PASS] MAP storage (scalar + message values) with explicit StructArray cache for message values.
- [PASS] NESTED_LIST storage (depth 2 and 3) with explicit leaf StructArray cache.
- [PASS] Null-safety (B2): all element nullability paths covered; no read-through-null.
- [PASS] field(i) no-slice: coordinate-consistent recursion via Accessor::Make.
- [PASS] No cross-file collector fork: single source of truth maintained.
- [PASS] Pure add-on (D-RBA-1): no regression; all prior test suites green.

---

## Final Verdict

**CONFORMANT — PASS**

RBA-4b implementation is ready to ship. All six conformance checks pass. Locked decisions D-RBA-1, D-RBA-4, D-RBA-6, D-RBA-7, and D-RBA-10 are satisfied. The full RBA feature set (RBA-1 through RBA-4b) covers all composite kinds and depths as designed.

---

## Test Status

- Accessor integration: 8/8 green.
- RBA-1 no-drift guard: green (storage-only change; existing output bytes unchanged).
- Full test suite: 70/70 green.
- Ready to merge.

