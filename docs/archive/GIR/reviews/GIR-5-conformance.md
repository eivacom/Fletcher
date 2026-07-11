# GIR-5 Conformance Review — Unified Schema IR Visitor

**Review Date:** 2026-07-10
**Review Type:** Static code + design conformance (working-tree uncommitted changes)
**Verdict:** CONFORMS

## Executive Summary

GIR-5 implementation successfully unifies the schema-source and in-process/IPC schema code paths onto one IR-driven visitor. Both `GenerateSchemaFunction` and `BuildMessageSchemaInto` are replaced with calls to a single `SchemaVisitor` that drives two sinks (C++ source and nanoarrow in-process), eliminating duplicated schema logic and guaranteeing byte-for-byte agreement through `.ipc` golden-file comparisons. All locked decisions are honored; no deviations or blocking issues detected.

---

## Pressure-Test Validation

### 1. Decision #5: Both Paths Unified onto ONE Visitor

**Status:** PASS

- **GenerateSchemaFunction** replaced with call to `GenerateSchemaFunctionFromIr`
- **BuildMessageSchemaInto** replaced with call to `BuildMessageSchemaIntoFromIr`
- Old 54-line and 88-line implementations removed entirely
- SchemaVisitor orchestrates single flatten walk
- CppSchemaSink renders source; NanoarrowSchemaSink executes in-process
- Both implement identical SchemaSink abstraction
- Visitor issues same sequence of sink operations regardless of sink type

### 2. .ipc / Schema Byte-Identity

**Status:** PASS

- 10 per-node-kind golden files present and spanning all required node kinds:
  - AllScalars (all scalar types)
  - Proto2Optional (nullable flag)
  - TimestampHolder, DurationHolder (WKT with NANO unit)
  - RepeatedScalarHolder, RepeatedStructHolder (List<Scalar/Struct>)
  - NestedStructHolder (singular nested struct)
  - MapScalarHolder, MapStructHolder (maps with scalar and struct values)
  - EnumHolder (enum lowered to INT32)
  
- Test harness `CppAndIpcByteIdentical` compares IR-driven schema to legacy golden via byte-for-byte equality
- RecordingSink trace comparison verifies both sinks receive identical operation sequence
- CaptureGoldens helper regenerates from legacy code when needed

### 3. Filter Preservation (UNSUPPORTED + FIXED_SIZE_LIST)

**Status:** PASS

- `IsSchemaRepresentable` predicate filters UNSUPPORTED and FIXED_SIZE_LIST (no throw)
- Filters non-representable LIST<LIST<Scalar>> but keeps LIST<LIST<...Struct>>
- Filters MAP with non-scalar key or invalid value
- `BuildFlattenedFieldListImpl` skips non-representable nodes (line 79)
- `EmitNodeType` switch has no FIXED_SIZE_LIST branch (defensive throw only)
- Matches ProjectIrToFieldMapping filter exactly
- Plugin succeeds silently on unsupported fields (defers error to GIR-8/#55)

### 4. Decision #1: Language-Neutral IR

**Status:** PASS

- IR carries abstract LogicalKind enum, never C++ type strings
- Type mapping (LogicalKind -> ArrowType) lives in C++ sink only
- `EmitScalarType` performs mapping at generation/execution time
- No Arrow-type or C++-type string fields added to IR nodes
- Header documented as language-neutral

### 5. Cosmetic Source Churn (Semantic Equivalence)

**Status:** PASS

- schema-> to schema.get()-> change is semantically identical
- Dropped "// Warning:" comments (not carried in IR; GIR-8 scope)
- List SetName reordering (SetType -> recurse -> SetName) matches design
- Mutation order preserved (type -> name -> nullable -> metadata)
- Byte-identical .ipc proves semantic equivalence

### 6. RBA Read-Only, FieldKind Retained, Bridge Temporary

**Status:** PASS

- GatherFields unchanged and available for other emitters
- FieldKind not deleted
- Schema visitor does not consume FieldMapping bridge; reads IR directly
- RBA accessor emitter unmodified
- Header updated to document schema visitor as read-only consumer

### 7. Tests Substantive (Not Tautological)

**Status:** PASS

- Golden files captured from pre-migration legacy code (fletcher::BuildMessageSchema)
- New visitor must independently produce identical output to legacy
- Stubbed visitor would fail immediately (red signal)
- RecordingSink proves sink agreement via trace comparison
- Byte-identity test is robust against hidden structural differences

---

## Design Conformance

All requirements from plans/GIR-5-unified-schema-ipc-visitor.md met:

- Flatten walk extraction: language-neutral, mirrors GatherFieldsImpl, builds dotted field_id
- SchemaSink abstraction: virtual interface with identical method set
- CppSchemaSink: renders nanoarrow calls as C++ source lines
- NanoarrowSchemaSink: executes calls in-process
- SchemaVisitor: orchestrates flatten walk, emits type/name/nullable/metadata in correct order
- Nested message struct handling: deep-copy + field overlay (overwrites name/metadata)
- List item naming: SetType, recurse, SetName("item") for all list shapes
- Map logic: entries, key/value children, struct values renamed "value"
- Metadata order: root {proto_package, proto_message}; field {field_number, field_id}
- Enum handling: INT32 + enum_identity (symbols deferred to GIR-9)

---

## Locked Decision Alignment

All locked decisions from plans/GIR-locked-decisions.md honored:

- #1 Language-neutral IR: LogicalKind enum only; C++ mapping in sink
- #2 Wire byte-identity: Golden .ipc files prove no change
- #3 RBA read-only: GatherFields available; FieldKind retained
- #4 Per-emitter migration: Guarded by .ipc golden + trace comparison
- #5 Schema unified: Both paths on one visitor
- #6 Fold fixes (unsupported): Filters silently; defers error to GIR-8
- #7 Enum first-class, Dictionary modifier: Handled correctly in IR and sink
- #8 Edge codec strategy: Schema scope only; codec out of scope
- #9 Red-first tests: Migration item guarded by oracle (not red-first)
- #10 Scope: Contained to protoc/ and tests/
- #11 Base after HARD: Confirmed

---

## Implementation Quality

- Header well-documented with GIR-5 explanation and locked-decision references
- Anonymous namespace cleanly separates flatten walk and helpers from public API
- IsSchemaRepresentable predicate covers all NodeKind cases
- Both sinks implement SchemaSink interface correctly
- CppSchemaSink uses std::deque for stable expression pointer storage
- SchemaVisitor orchestration logic clear and matches design specification
- Test fixtures comprehensive; RecordingSink decorator captures operation traces
- CMakeLists.txt updated to compile new .cpp and run new test
- Golden directory properly populated with 10 binary files

---

## No Blocking Issues

**CONFORMS to GIR-5 design and all locked decisions.**

All pressure-test items pass. Implementation ready for merge pending CI green.

Review file: plans/reviews/GIR-5-conformance.md
