# Codex Review: GIR-7 TypeScript Interface & SchemaDescriptor Migration

**Target:** branch diff against cad2ddd5960eb3e39b7e53cf29bded65e7ec2f2f

**Review Status:** Complete

## Summary

The new TS visitor can mis-render singular flattened wrappers that expand to nested struct lists, changing supported generated TypeScript types and descriptors from the prior emitter. The coverage golden does not include this input shape, so the regression can slip through.

## Findings

### P2: Wrapper-Name Propagation Through Nested Lists

**Location:** `protoc/src/ts_backend_visitor.cpp:113-113`

**Issue:** For a singular flattened wrapper whose IR is a nested list (e.g., `NestedStructListWrapper foo` where the wrapper contains `repeated StructListWrapper`), the code carries `declared_struct` through every list level. This causes the leaf struct to render as the outer wrapper type:
- **Actual (broken):** `INestedStructListWrapper[][]` / `NestedStructListWrapper.fields`
- **Expected (prior behavior):** `ILeaf[][]` / `Leaf.fields`

This changes already-supported struct-leaf nested-list TypeScript output and produces an incorrect runtime descriptor.

**Root Cause:** The wrapper override (setting the rendered struct to the wrapper's declared leaf struct) is being applied at all list nesting levels, not just when the immediate list element is the flattened wrapper's leaf struct.

**Fix Required:** Only propagate the wrapper's declared struct when descending into the first list level; reset or condition the override for nested list containers.

**Impact:** This is a byte-identity regression for any message containing a singular flattened wrapper that nests list-of-list structures. The golden test (`coverage.fletcher.ts`) does not currently exercise this pattern, allowing the defect to remain undetected.

---

## Review Scope Checklist

The review focused on the following aspects per the stated requirements:

- **TS visitor + ts_backend correctness:** Identified a critical type-rendering defect in wrapper propagation for nested lists; other scalar, list, struct, map, and WKT handling appears structurally consistent with prior emitter.
- **Wrapper-name recovery logic:** Found defect in gating logic for singular-message + IsFlattenedWrapper case when nested lists are involved.
- **GenerateFile details:** Basic structure (message ordering, imports) appears preserved; deeper validation limited by tool policy restrictions.
- **Committed golden:** The golden file (`coverage.fletcher.ts`) does not include the problematic pattern (nested lists within singular flattened wrappers), which allowed the defect to slip through.
- **ValidateServiceMethod relocation:** No callers or linkage issues detected from available inspection.
- **Dead-code cleanup:** No dangling references identified in processed files.
- **Test substance:** The test `TsVisitor.DescriptorByteIdentical` will catch byte-identity regressions for exercised patterns, but does not cover the nested-wrapper case.

---

## Recommendation

Before landing GIR-7:
1. Add a test fixture to `coverage.proto` (or test input) that exercises a singular flattened wrapper containing nested lists.
2. Verify the generated TypeScript output renders the leaf struct type correctly (not the outer wrapper type).
3. Fix the wrapper propagation logic in `ts_backend_visitor.cpp` to respect list-nesting boundaries.
4. Re-run the byte-identity golden-diff test to confirm the fix restores parity with the prior emitter.

---

**Review Date:** 2026-07-10  
**Reviewer:** Codex Code Review System  
**Method:** Static analysis against branch diff
