# GIR-9 Architecture Conformance Review

**Date:** 2026-07-11  
**Reviewer:** Codex (adversarial review)  
**Target:** Branch diff against 9b6e36eb9588e9c7e56f03cd2a3f7f4444220de7  
**Design:** plans/GIR-9-enum-symbols.md  
**Locked Decisions:** plans/GIR-locked-decisions.md  

## Verdict
**DEVIATES** — Blocking conformance issue identified

## Critical Findings

### [HIGH] Nested Enum Owners—Skipped Messages Can Reference Undeclared Enums

**Status:** No-ship blocker

**Issue:**
Enum-owner dependency handling assumes every enum owner has a generated C++ class, which is not true for skipped messages. The implementation can emit typed accessors naming enum classes that GIR deliberately omits, resulting in compile-breaking generated code.

**Technical Details:**
- Path affected: `protoc/src/generator.cpp:182-187`
- When a generated message has a field of type `SkippedOwner.InnerStatus` (nested enum), the typed accessor generation spells `SkippedOwner::InnerStatus`
- However, `SkippedOwner` and its nested enum are never emitted because:
  - Nested enum declarations are only emitted inside `GenerateMessageClass`
  - `TopologicalVisit` can drop recursive owners without adding them to output order
  - `GenerateFile` also skips flattened wrappers
  - The enum-owner dependency recursion blindly recurses to any same-file owner without checking emitability

**Conformance Violation:**
- Violates GIR-9 Step 2 design: "every file-level enum → `enum class : int32_t`" and "typed accessors use the owning C++ enum declaration"
- Weakens safety contract: generates code that will not compile
- Scope/guard weakening: the emitability predicate used by `GenerateFile` is not applied to typed accessor generation

**Test Coverage Gap:**
Red-first test is missing: no fixture exists where a generated message references a nested enum owned by a recursive or flattened/skipped message. This is a genuine compile-red scenario that the design requires to be handled.

## Recommendations

**Required before merge:**

1. **Prove Enum-Owner Emitability:**
   Before emitting a typed accessor for a nested enum, verify that the enum owner will have a generated C++ class. Use the same emitability predicate (`GenerateFile` / `GenerateViewFile` checks) that gates message class emission.

2. **Choose a Safe Path:**
   - **Option A:** Emit a minimal enum-bearing shell for skipped owners (safest, consistent with "all enums emitted")
   - **Option B:** Suppress typed accessor generation with a clear diagnostic error when an enum owner is skipped
   - **Option C:** Reject inputs at descriptor validation time that would create this situation (restrictive but safe)

3. **Add Red-First Coverage:**
   Add a test fixture to `integration-tests/protoc-coverage/coverage.fletcher.proto` or `enum_coverage.fletcher.proto` where:
   - A message in the coverage fixture is generated
   - That message has a field referencing a nested enum
   - The nested enum is owned by a message that is skipped/flattened
   - Verify that without the fix, compilation fails with a clear error (enum class not found)
   - Verify that with the fix, compilation succeeds

4. **Gate Accessor Generation:**
   Refactor the call sites in `generator.cpp` that emit `CppEnumName` references to check enum-owner emitability before proceeding. This parallels the design's enforcement that only emitted enums get typed accessors.

## Pressure-Test Assessment

| Item | Status | Notes |
|------|--------|-------|
| **1. No wire change / byte-identical goldens** | PASS | Coverage fixture is new; existing goldens untouched. Int32 storage retained. |
| **2. Emit ALL enums + scope** | FAIL | Typed accessors can reference undeclared enums from skipped owners. |
| **3. Enum-owner ordering edge** | INCOMPLETE | Topological ordering added, but only if owner is later emitted. Cannot verify full correctness without fixing issue #2. |
| **4. RBA read-only** | PASS | RBA sources untouched; enum generation correctly omits Rust (no `__rba.fletcher.rs` re-emission). |
| **5. Deviations—Acceptable per design** | INCOMPLETE | (a) `CppEnumName` in `cpp_backend_type_table.*` — acceptable signature variance; (b) `validate_generated_ipc.cmake` stem-aware — guards still validate full message set (not weakened); (c) enum golden omission — in-memory round-trip with forced comparison is a sufficient wire guard for new fixture. |
| **6. Red-first genuine** | FAIL | Missing: nested-enum-owned-by-skipped-message fixture that compiles red without the fix. |

## Summary

The design requires that all enums be emitted and typed accessors reference only declared enums. The implementation violates this by assuming enum owners always generate classes. This creates a compile-breaking gap for valid descriptors.

**Action:** Fix enum-owner emitability before merge. Add red-first test to prevent regression.

---

*Review methodology:* Static analysis of diff against design and locked decisions. No re-run of test suite (green: protoc 53/53, coverage 14/14, RBA no-drift).


---

## Targeted Re-Review (2026-07-11): Fix Verification

**All three findings CONFORMS to design:**

### 4a (Owner-Emittability Gate) — RESOLVED ✓

**Gate implementation:** `CppEnumTypeEmittable(ed)` in `cpp_backend_type_table.cpp:158-164`
- Returns `true` for top-level enums
- Returns `!IsRecursive(owner) && !IsFlattenedWrapper(owner)` for nested enums
- Mirrors emit-loop skip predicate exactly

**Applied at all 7 typed-accessor generation sites:**
- Row (4): EmitSetters singular/nullable, EmitGetters singular/nullable, repeated, map
- View (3): EdgeViewGetterVisitor scalar, repeated, map

**Red-first fixtures present:**
- FlattenedEnumRef (references FlattenedEnumOwner.WrappedStatus — owner is flattened wrapper, never emitted)
- RecursiveEnumRef (references RecursiveEnumOwner.RecStatus — owner is recursive, skipped)
- Both compile only when gate suppresses typed accessors (raw int32 only)

### P2-1 (Flattened Imported Enum Header) — RESOLVED ✓

**Descent implementation:** `CollectCrossFileEnumIncludesFromMessage` (lines 127-147)
- Line 136-139: Descends field-level flatten, recurses into flattened message's enum includes
- Line 141-143: Collects cross-file enum includes for inlined fields

**Fixture:** FieldFlattenedEnum field-flattens ImportedEnumCarrier (which carries imported TopLevelStatus)
- Inlined enum's header include collected → enum_coverage.fletcher.pb.h includes coverage.fletcher.pb.h
- Test verifies `ff.carrier_status_typed()` compiles

### P2-2 (Cycle Guard) — RESOLVED ✓

**Cycle guard implementation:** `TopologicalVisit` threads shared `visiting` set
- Line 169: Early return on `visiting.count(msg)`
- Line 180: Insert message into visiting before recursion
- Line 184, 193, 195, 214: Thread visiting through all recursive calls (nested, message deps, enum-owner deps)
- Line 217: Erase from visiting after processing

**Fixture:** RecursiveEnumOwner self-references, also declares enum
- Enum-owner edge can form cycle on non-recursive messages
- Without guard: stack overflow; with guard: early return on visiting check

**Ordering invariant:** Acyclic case unchanged (visiting never triggers cycle branch)

### No Regression ✓

- **Storage:** int32 (TS golden confirms WireTypeId.INT32, no wire change)
- **Existing enums:** gain typed accessors additively (coverage.proto fields tested)
- **RBA:** untouched (enum_coverage omits `rust` to avoid duplicate __rba output)
- **IR nodes:** descriptor-based only (no C++ strings, locked #1)

**Verdict:** PASS — All blocking items resolved, fixtures genuine, no regressions detected.

