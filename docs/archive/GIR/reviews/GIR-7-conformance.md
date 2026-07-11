# Codex Adversarial Review: GIR-7 Conformance

**Target:** Working tree diff (GIR-7 changes)  
**Verdict:** needs-attention  
**Date:** 2026-07-10

## Executive Summary

**No-ship: GIR-7 leaves a TS generation path on the FieldMapping bridge, violating the migration contract.**

The TypeScript backend visitor still consumes FieldMapping for import discovery, contradicting the architectural claim that TS is migrated to IR-direct consumption after GIR-7. This is a **blocking conformance deviation** from the design.

## Findings

### High-Severity Conformance Issue

**TS visitor still consumes FieldMapping for import discovery** 
- **Location:** `protoc/src/ts_backend_visitor.cpp:216-239` in `TsVisitor::GenerateFile()`
- **Description:** The TS visitor generates messages from `BuildFlattenedFieldList`, but its cross-file import scan still calls `MapField()` and switches on `FieldKind`. This violates the GIR-7 bridge contract.
- **Contract Violation:** 
  - Design claims: FieldMapping remains only for RBA + edge row-class emitters until RIR
  - TS should be direct on IR after GIR-7
  - Actual behavior: TS backend maintains FieldMapping dependency for import discovery
- **Risk Profile:**
  - **Immediate:** Cross-file TS output can drift from the IR-rendered field set when the bridge rejects or classifies a field differently
  - **Long-term:** Hidden dependency that breaks when FieldMapping is narrowed or removed in RIR
- **Recommendation:** Replace the import scan with an IR-based walk over the same message/order model used for emission (e.g., `BuildFlattenedFieldList` plus recursive inspection of `IrNode` struct/map/list identities). Remove the `MapField`/`FieldKind` dependency from the TS backend entirely.

## Pressure-Test Results

1. **Language-neutrality (IR untouched):** Partial pass — IR core is untouched, but TS backend does not achieve IR-direct design intent
2. **Byte-identity:** Not verified (requires re-test after FieldMapping removal)
3. **Wrapper-name correctness:** Not verified
4. **Flatten walk:** Uses `BuildFlattenedFieldList` for emission, but import scan violates this pattern
5. **Parked cases:** Not examined
6. **Scope/bridge:** DEVIATION — TS still consumes FieldMapping; contract violated
7. **ValidateServiceMethod relocation:** Not examined

## Next Steps

- **Re-implement the TS import scan** to use IR-native structures instead of FieldMapping
- **Re-run TsVisitor.DescriptorByteIdentical** test to confirm byte-identity after removal
- **Re-run all TS compile checks** (existing fixtures)
- **Resubmit for conformance review**


---

## Step-3 Re-Review: GIR-7 Fixes for Findings 4a & 4b (2026-07-10)

**Target:** Staged fixes for prior blocking (4a) + code-review P2 (4b)  
**Verdict:** CONFORMS

### Finding 4a Verification (Blocking — TS De-Bridged)

✅ **RESOLVED.** `TsVisitor::GenerateFile()` and the entire TS backend now walk the IR using:
- `cpp_backend::BuildFlattenedFieldList()` for message field iteration (lines 216, 263)
- `CollectStructImportFiles()` for recursive IR node inspection (lines 81-104)
- No `MapField()`, `FieldKind`, or `FlattenLeafStruct` anywhere in `ts_backend_visitor.cpp`

Generator.cpp migration is clean: all dead TS-specific code removed (`GenerateTsMessage`, `TsFieldType`, `FlattenLeafStruct`, `DotToSlash`), replaced with single-line call `ts_backend::TsVisitor(file).GenerateFile()` (line 1378).

Byte-identity preserved:
- Interface names via language-neutral `TsInterfaceName(Descriptor*)`
- Descriptor literals with hardcoded `nullable: false` for nested elements (line 194)
- Element/mapKey/mapValue shapes match prior emitter
- Cross-file imports ordered lexically via `std::set` (line 259)

TS is now fully IR-direct. Decision #4 (RBA read-only bridge) unaffected: RBA retains thin FieldMapping projection; only TS consumer dropped.

### Finding 4b Verification (Wrapper Propagation — P2 Guard)

✅ **RESOLVED.** `declared_struct` correctly threads ONLY to direct STRUCT elements:

**InterfaceType (lines 136-145):**
- List element: `elem_declared = (elem.kind == STRUCT) ? declared_struct : nullptr`
- Recursion drops declared wrapper at first nested-list level

**AppendComposite (lines 162-189):**
- Same gate: declared struct only for direct-STRUCT elements

**Test assertion (test_ts_visitor.cpp:94-111):**
- Fixture with `StructListWrapper` (singular→single list) + `NestedStructListWrapper` (singular→nested list)
- Confirms: `single: IStructListWrapper[]` (wrapper name kept) ✓
- Confirms: `nested: ILeaf[][]` (leaf identity, wrapper NOT leaked) ✓
- Asserts: `INestedStructListWrapper[][]` absent ✓

This is a **genuine, non-vacuous guard** — it pins the critical discriminator (singular-single vs singular-nested) that the byte-identity golden cannot exercise. The test ensures the wrapper name does not propagate through list nesting.

### Locked Decision Conformance

- **#1 (language-neutral IR):** ✓ No TS strings on IR; `TsLookupScalar/TsInterfaceName/TsSchemaConstName` own all type text
- **#3/#4 (RBA read-only):** ✓ RBA untouched; FieldMapping remains thin projection
- **#8 (descriptor-driven codec):** ✓ Descriptors still generated (line 237, ElementDescriptor)

### New Issues

None detected. Code is clean; no dangling references or bridge violations.

### Recommendation

**APPROVE.** Both findings resolved; locked decisions honored; genuine test assertion guards P2 branch.

