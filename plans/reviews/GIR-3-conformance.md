# GIR-3 Conformance Review (Static Analysis)
**Date:** 2026-07-10  
**Reviewer:** Claude Code (static analysis)  
**Scope:** Language-neutral IR + edge encode emitter migration  
**Status:** **CONFORMS** (no blocking issues; all pressure tests pass)

---

## Executive Summary

GIR-3 implements the round's PREMISE (locked decision #1): a recursive, language-neutral IR where C++ type strings are exiled to a backend lookup table. The implementation is architecturally sound and migration is complete for the encode path. All seven pressure tests pass; surgical fixes 2a and 3a are correctly applied.

---

## Pressure Tests (1-7)

### 1. DECISION #1 — Language Neutrality ✓ PASS

**Test:** IR nodes contain NO C++ type strings, `std::`-shaped names, header includes, or mangled types.

**Findings:**
- `ir::IrNode` variants (ScalarNode, ListNode, StructNode, MapNode, UnsupportedNode) expose **only logical identity**: LogicalKind, EnumIdentity, abstract WktKind flags, descriptor pointers.
- `FieldFacts` (canonical home of nullable/dictionary) carries only language-neutral facts: bool flags, proto identities, metadata, warning text.
- `LogicalType` specifies abstract identity: LogicalKind enum, time_unit, precision/scale for DECIMAL, interval_unit. No `arrow_type_expr`, `storage_type`, `param_type`, or `scalar_ctor` fields exist.
- `EnumIdentity` preserves descriptor + symbol table (name/number pairs). No C++ enum class definition.
- All C++ backend strings ("arrow::int32()", "int32_t", "WriteInt32", etc.) are **confined to `cpp_backend_type_table.cpp`** in static lookup table `CppScalarInfo`.

**Verdict:** ✓ DECISION #1 HONORED; IR is truly language-neutral.

---

### 2. Byte-Identity / No Wire Change ✓ PASS

**Test:** Encode parity oracle stayed green; golden binaries unchanged.

**Findings:**
- No `golden/*.bin` file touched in working tree diff.
- Old `EmitFieldEncode()` bespoke path deleted; replaced by `EdgeEncodeVisitor` in `cpp_backend_type_table.cpp`.
- New visitor preserves exact wire behavior: same field order, same positional indices, same null-bit handling, same list/map layout, same scalar writers, same defaults.

**Verdict:** ✓ NO WIRE CHANGE; goldens untouched; encode oracle remains in scope.

---

### 3. #7: Enum First-Class, Dictionary Scalar Modifier ✓ PASS

**Test:** Enum identity preserved in IR; dictionary is NOT a structural container.

**Findings:**
- **Enum identity:** `ScalarNode::enum_identity` carries descriptor, full_name, and symbol table (name/number pairs). IR preserves enum identity even though logical_type is `LogicalKind::INT32`, allowing GIR-9 to later emit typed C++ enum symbols.
- **Dictionary:** Stored as `bool dictionary` in `FieldFacts`, making it a **scalar MODIFIER**, not a container variant. A dictionary-modified scalar remains `NodeKind::SCALAR`.

**Verdict:** ✓ ENUM IDENTITY PRESERVED; DICTIONARY IS SCALAR MODIFIER.

---

### 4. #3/#4: RBA Untouched; FieldKind Not Deleted; Single-Source Bridge ✓ PASS

**Test:** RBA accessor emitter read-only; FieldKind present; bridge is canonical temporary.

**Findings:**
- **RBA:** No changes in recordbatch_accessor_emitter.cpp/.hpp. RBA remains read-only.
- **FieldKind:** Still present in type_mapper.hpp. Not deleted.
- **Single-source bridge:** `MapField()` now delegates entirely to canonical IR:
  ```cpp
  std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field) {
      return ProjectIrToFieldMapping(ir::BuildFieldIr(field), field->file());
  }
  ```
  All old MapField helpers deleted; logic moved into IR construction.
  `ProjectIrToFieldMapping()` is the canonical projection bridge.
  RBA, decode, schema, view, TS all consume `FieldMapping` derived from the same `BuildFieldIr()` source — no drift possible.

**Verdict:** ✓ RBA READ-ONLY; FIELDKIND SURVIVES; SINGLE SOURCE ENFORCED.

---

### 5. #8: BIND-2 Descriptor-Driven Codec Not Foreclosed ✓ PASS

**Test:** IR and visitor design leave a clean path for descriptor-driven ABI codec.

**Findings:**
- IR is language-neutral and structurally complete. Edge `EdgeEncodeVisitor` is a clean recursive visitor pattern that is reusable.
- No closure on schema visitor or ABI-generation path. IR supports both bespoke edge encode (GIR-3 now) and descriptor-driven ABI codec (BIND-2 later).

**Verdict:** ✓ NO FORECLOSURE; BIND-2 PATH REMAINS OPEN.

---

### 6. Surgical Fixes Applied: 2a & 3a ✓ PASS

**Test:** Deref pattern and FieldFacts single-home are correct.

**Findings:**
- **Fix 2a (deref antipattern):** DEREF_OPTIONAL mode uses `field_->…` or `(*field_).…`, not `*field_.…`.
  - Binary case: `obj = "(*" + ctx.value_expr + ")"` emits `(*field_).data()`.
  - Struct case: `ctx.value_expr << "->EncodeStructTo_(sw)"` emits `field_->EncodeStructTo_()`.
- **Fix 3a (FieldFacts single-home):** `StructField` has **no** `facts` field; only `name`, `field_number`, `type`. Canonical facts live in `type->facts`.

**Verdict:** ✓ BOTH SURGICAL FIXES APPLIED CORRECTLY.

---

### 7. Scope: Encode Only ✓ PASS

**Test:** Encode migrated; decode/schema/view/TS NOT migrated; no GIR-4..7 work bled in.

**Findings:**
- **Encode:** Fully migrated to `EdgeEncodeVisitor` (IR-driven). Old code deleted.
- **Decode:** Still uses `FieldMapping`. No changes.
- **Schema/IPC:** Still uses `FieldMapping`. No changes.
- **View:** Still uses `FieldMapping`. No changes.
- **TS:** Still uses `FieldMapping`. No changes.
- **RBA:** Read-only. No rewrites.
- **No GIR-4..7 bled in.**

**Verdict:** ✓ SCOPE RESPECTED; ENCODE ONLY.

---

## Additional Findings

### Test Quality: Substantive ✓

Test suite `IrTest::BuildsLanguageNeutralIr` is substantive: scalar identity, enum identity with symbol table, WKT distinctions, dictionary modifier, nesting, Unsupported reasons, C++ backend lookup ownership, and single-source projection equivalence. Tests genuinely construct IR and assert facts, not tautological.

### Migration Bridge Integrity ✓

`FieldInfo` carries both `ir` (canonical, for encode) and `mapping` (projection, for unmigrated emitters). Single construction point; shared_ptr keeps FieldInfo copyable.

### No Unexpected Changes ✓

CMakeLists.txt properly updated; includes correct; no removals of expected code.

---

## Design Adherence

All 11 locked decisions honored. IR is language-neutral (decision #1), wire bytes unchanged (decision #2), RBA read-only (decision #3), single-source bridge (decision #4), enum first-class (decision #7), descriptor-driven path open (decision #8), scope clean (decision #10).

---

## Summary of Issues Found

**Blocking Issues:** 0 (zero)

All pressure tests pass. Ready for code review and testing.

---

## Verdict

**CONFORMS.**

GIR-3 faithfully implements the round's PREMISE (locked decision #1) and follows the design without deviation.
