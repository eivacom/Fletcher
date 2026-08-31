# GIR-5 Code Review: Unified Schema + IPC Visitor

**Reviewer:** Claude Code (Codex static analysis)
**Date:** 2026-07-10
**Status:** 1 BLOCKING issue; implementation is architecturally sound.

## Summary

GIR-5 unifies the two hand-kept-in-lockstep schema emitters onto a single IR-driven SchemaVisitor with two sink implementations, eliminating drift risk while preserving byte-identical .ipc output via golden-file comparisons.

The implementation is architecturally sound. However, **one critical blocking issue** must be resolved.

---

## BLOCKING ISSUE: Untracked Files

**Problem:** CMakeLists.txt references three untracked new source files:
- protoc/src/cpp_backend_schema_visitor.cpp
- protoc/include/cpp_backend_schema_visitor.hpp
- protoc/tests/test_schema_visitor.cpp

When applied to base branch, these files will be missing, causing compilation to fail.

**Fix:** Stage files into git before merge:
```
git add protoc/include/cpp_backend_schema_visitor.hpp \
        protoc/src/cpp_backend_schema_visitor.cpp \
        protoc/tests/test_schema_visitor.cpp
```

---

## VERIFICATION CHECKLIST

### 1. Flatten Walk Correctness [PASS]

BuildFlattenedFieldList (cpp_backend_schema_visitor.cpp lines 61-88) reproduces GatherFieldsImpl exactly:
- Field-level flatten via HasFieldFlatten(fd) with recursive path extension (line 73-74)
- Dotted field_id path building (lines 66-68)
- IsSchemaRepresentable() filter (lines 31-59) matches type_mapper.cpp:150-152:
  - Skips UNSUPPORTED and FIXED_SIZE_LIST
  - Rejects List<Scalar>, List<List<Scalar>> as non-representable
  - Accepts List<Struct>, List<List<...Struct>>
  - MAP: scalar key + scalar/struct value only

Preserves byte-identical schema output.

### 2. Both Sinks Emit Identical Schema [PASS]

**CppSchemaSink** (lines 164-240): emits nanoarrow calls as C++ source text
**NanoarrowSchemaSink** (lines 246-316): executes nanoarrow calls in-process
Both implement SchemaSink interface identically.

**Per-Node-Kind Verification:**
- Scalar: SetTypeScalar() only
- List: SetTypeList() -> Recurse -> SetName("item") after recursion
  - Handles List<Scalar>, List<Struct>, List<List<Struct>>
- Struct: DeepCopyMessageStruct()
- Map: SetTypeMap -> entries -> key/value
  - Scalar key: default "key" name
  - Scalar value: default "value" name
  - Struct value: DeepCopy then SetName("value") to override

**Forcing Test Proof (test_schema_visitor.cpp):**
- RecordingSink decorator (lines 283-353) captures ordered operation stream
- CppAndIpcByteIdentical (line 377): TraceCpp == TraceNano [PASS]
- Serialized .ipc bytes match committed golden [PASS]

### 3. "item" Restore After Deep-Copy [PASS]

List emission (lines 384-393):
1. SetTypeList() first
2. Recurse into element
3. SetName("item") after recursion

Handles all shapes: List<Scalar>, List<Struct>, List<List<Struct>>
Matches base behavior (generator.cpp lines 973-996).

### 4. Cosmetic Source Churn [PASS]

- Schema pointer: schema.get() unchanged
- Warning comments: intentionally dropped (IR carries no warning field); documented in design
- Field overlay order: type -> name -> nullable -> metadata (same)
- SetTypeStruct order: ArrowSchemaInit then SetTypeStruct (unchanged)

### 5. Golden Mechanics [PASS]

**10 .ipc Files Covering All Node Kinds:**
AllScalars, Proto2Optional, TimestampHolder, DurationHolder, RepeatedScalarHolder, NestedStructHolder, RepeatedStructHolder, MapScalarHolder, MapStructHolder, EnumHolder

**Regeneration Guard:**
- Requires explicit FLETCHER_CAPTURE_GOLDENS=1 env var
- Uses legacy BuildMessageSchema() as ground truth
- Writes to committed source directory
- Cannot silently overwrite in CI
- Test skips by default

### 6. Memory Safety & Nanoarrow Lifetime [PASS]

**Deep-Copy Management:**
- CppSchemaSink: emits text only
- NanoarrowSchemaSink: temporary UniqueSchema (line 313), recurse, deep-copy, destroy
- All error-checked with CheckNa()

**Metadata Building:**
- ArrowBufferInit / ArrowMetadataBuilderInit / Append error-accumulated
- ArrowBufferReset() called unconditionally
- Final error check

**No Leaks/Dangling Refs:**
- SchemaRef opaque handles cast correctly
- Child pointers owned by parent
- CppSchemaSink uses deque for string stability

### 7. Dead-Code Cleanup [PASS]

**Removed (Thin Wrappers):**
- GenerateSchemaFunction body (73 lines) -> 2-line wrapper
- BuildMessageSchemaInto body (95 lines) -> 1-line wrapper

**Retained:**
- GatherFieldsImpl (called by RBA accessor emitter)
- EmitNanoarrowTypeSetup (called elsewhere per scope)
- SetScalarSchemaType (called elsewhere per scope)

---

## LOCKED DECISION COMPLIANCE

✓ #5: One visitor, both sinks
✓ #1: Language-neutral IR (LogicalKind->nanoarrow in sink, not IR)
✓ #3: RBA read-only (GatherFields called RO, FieldKind retained)
✓ #4: Schema reads IR directly, not FieldMapping bridge
✓ #6: Unsupported->build-error deferred to GIR-8
✓ #8: BIND-2 codec constraint open

---

## RECOMMENDATION

**APPROVE with blocking issue resolved.** Implementation is architecturally sound and meets all locked decisions. The untracked-file issue is a straightforward `git add` fix. Once resolved, ready for merge.

