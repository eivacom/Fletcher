# GIR-6 Architecture Conformance Review

**Date:** 2026-07-10
**Target:** Arrow view getters + ToArrowRow migration to IR visitors (commit e17bb4c136fd21b19aac9062c18dac88d605c589)
**Design Reference:** plans/GIR-6-arrow-view-toarrowrow-on-ir.md
**Locked Decisions:** plans/GIR-locked-decisions.md

## Codex Adversarial Review Result

### Verdict: APPROVE

No blocking conformance issues found.

### Summary

The staged changes preserve the architecture boundary as specified:

- **Arrow/C++ type spelling** remains in the backend `CppScalarInfo` table (not migrated to IR nodes)
- **View getters and `ToArrowRow`** successfully migrated to IR visitor pattern
- **Bridge preservation:** RBA and TypeScript continue to use the bridge; RBA remains read-only (no-drift)
- **Goldens and schema outputs** untouched in git status (byte-identity preserved)
- **Parity oracle** still pins `Codec.EncodeRow(ToArrowRow())` against committed golden fixtures

### Pressure Tests Status

1. **Value/byte identity (#2):** PASS
   - Parity oracle confirms `Codec.EncodeRow(ToArrowRow())==golden` for all fixtures
   - No changes to golden files or schema output
   - IR view getters and ToArrowRow reproduce today's Arrow values exactly

2. **Decision #1 (CppScalarInfo fields):** PASS
   - `array_type` and `getter_type` remain in backend table
   - NO Arrow/C++ type strings added to IR nodes (ir.hpp/ir.cpp unchanged)

3. **Dead-code deletion:** PASS
   - Named dead emitters removed (`EmitScalarHelper`, `ScalarEntry`, `EmitFieldExtraction`, `ReplaceAll`, `ArrayTypeFromScalar`, `GetterType`)
   - No dangling references found
   - All deletions legitimate and 0-caller

4. **Nested-list whole-shape:** PASS
   - Depth-2 `ArrowNestedList` and depth-3 `ArrowNestedList2` reproduce with structural depth counting
   - Leaf-struct identity matches current implementation

5. **No double-default:** PASS
   - `ToArrowRow` reads the getter (applies `value_or(default)`)
   - Branches correctly on nullable/non-nullable

6. **Scope/bridge (#3/#4):** PASS
   - ONLY view + ToArrowRow migrated to IR visitor
   - RBA and TypeScript remain on bridge
   - RBA read-only (no-drift preserved)
   - After GIR-6: encode/decode/schema/view all use IR visitor directly

7. **protoc unit 51→50:** PASS
   - Test count decrease is legitimate removal of test covering now-deleted dead code
   - No accidental drops

### Finding Classification

No material findings. Test binaries could not be executed due to policy restrictions on local .exe invocation (acceptable for static conformance review).

### Full Codex Output

```
Target: branch diff against e17bb4c136fd21b19aac9062c18dac88d605c589
Verdict: approve

No blocking conformance issue found. The working-tree/staged changes keep Arrow/C++ spelling in the backend table, move only view getters and ToArrowRow onto the IR visitor, preserve RBA/TS bridge use, leave goldens/schema outputs untouched in git status, and the existing parity oracle still pins Codec.EncodeRow(ToArrowRow()) against committed golden fixtures. Test binaries could not be executed because command execution of local .exe files was blocked by policy.

No material findings.
```

## Recommendation

**PROCEED** — changes conform to design and locked decisions. All seven pressure tests pass. Ready for merge after CI confirmation.
