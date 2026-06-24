# Code Review: RBA-5 Rust Scalar Accessor Implementation

**Reviewed against:** e630e09
**Review date:** 2026-06-24
**Status:** RESOLVED - Temporal coverage verified 2026-06-24

## Summary

RBA-5 fills EmitRustAccessor in protoc/src/recordbatch_accessor_emitter.cpp to emit fully-functional Rust accessor structs for scalar-only proto messages. The implementation includes dual factory methods with offset preservation, type-only validation, fully-qualified paths, and correct getter signatures. A new Cargo test crate with build.rs module assembler, WKT include auto-discovery, and comprehensive fixtures (including temporal fields) complete the work.

## Findings by Severity

### Blocking (1)

**[B1] Missing CI workflow file in git tracking**

The .github/workflows/ci.integration-test.protoc-gen-fletcher-rust.yml is untracked but referenced in ci.pr.yml:424. This will cause PR CI failure.

Location: .github/workflows/ci.integration-test.protoc-gen-fletcher-rust.yml (untracked)
Impact: PR merge fails with "workflow not found" error
Status: PM staging for commit; review verdict disregards per coordinator note

---

### Major (RESOLVED)

**M1 RESOLVED: Temporal field coverage gap**

Resolution: telemetry.proto now includes google.protobuf.Timestamp occurred_at and google.protobuf.Duration elapsed, mapping to TimestampNanosecondArray/DurationNanosecondArray with getter -> i64.

Verification:
- build.rs auto-derives WKT include path from protoc (hermetic)
- Generated code produces: TimestampNanosecondArray, DurationNanosecondArray
- Type-gated downcasts with correct DataType::Timestamp/Duration
- Test assertions validate per-row reads round-trip on RecordBatch and non-zero-offset StructArray
- Cargo test 4/4 pass, all temporal assertions executed

---

### Minor (3)

**[m1] WKT include auto-discovery adds complexity**

build.rs wkt_include_dir() derives path from protoc (standard layout). Hermetic, documented, acceptable.

**[m2] Absolute paths in generated code**

Platform-specific paths in OUT_DIR; no impact.

**[m3] Temporal unit parsing assumes specific expression form**

Simple parenthesis matching; low risk for current grammar.

---

### Nits (2)

Comments could clarify RBA-6 metadata and BTreeMap ordering.

---

## Test Coverage

Cargo test 4/4 pass, exercising all scalar types:
- Float64, Int32, String, Binary (nullable variants)
- Timestamp (Nanosecond unit)
- Duration (Nanosecond unit)

All major paths tested:
- RecordBatch factory with validation
- StructArray factory with non-zero offset child-slicing (D-RBA-7)
- Type mismatch error handling (never panics)
- Name/flag tolerance
- Temporal round-trip (3 rows validated)

---

## Implementation Quality

**Factories:** Proper offset preservation, child-slicing for from_struct, no panics
**Getters:** Correct signatures (scalar->value, nullable->Option, utf8->&str, binary->&[u8])
**Type mapping:** All scalars supported; temporal units parsed dynamically
**Module assembly:** Same-package grouping with keyword sanitization
**Downcast:** Offset-preserving, not re-Arc'd
**Arrow pin:** Correctly pinned 59.0.0 with justification

---

## Recommendation

RBA-5 is substantially correct and well-engineered. Sound validation, proper error handling, correct offset preservation, comprehensive test coverage including temporal fields now verified end-to-end.

Status: Ready for merge once blocking issue is committed (PM staging per coordinator).

