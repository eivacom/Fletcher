# GIR-1 Conformance Review
**Item:** GIR-1 Phase 3a greenfield compile-and-run harness  
**Design:** plans/GIR-1-compile-and-run-harness.md  
**Locked Decisions:** plans/GIR-locked-decisions.md  
**Base:** aa207ddd9404c89b71dbf2b51e576922df79e338  
**Date:** 2026-07-10

---

## Verdict: CONFORMS

The implementation faithfully satisfies the GIR-1 design and respects all locked decisions.

---

## Pressure-Test Results

### 1. Actual COMPILE + EXECUTE of Generated Code

**test_coverage_harness.cpp:**
- Constructs CompositeCoverage row via fixture (fx::MakeComposite())
- Encodes: EncodedRow encoded = row.Encode()
- Reconstructs: CompositeCoverage decoded(encoded) using generated encoded-row constructor
- Verifies 15+ representative values: scalars, strings, optionals, WKT, enums-as-int32, nested structs, repeated lists, maps, flatten
- Arrow side: ToArrowRow called; view constructed and values read
- IPC schema: Opens .ipc files, deserializes, validates as Arrow schema

**test_coverage_accessor.cpp:**
- Builds RecordBatch via generated ToArrowRow
- Constructs RBA accessor: CompositeCoverageAccessor::Make(batch)
- Reads values: scalar through nested struct, optional structs, repeated, maps, nested lists (depth <= 2 cap)

**CMakeLists.txt:**
- Single add_custom_command with --fletcher_opt=ipc,accessor,ts,rust emits all six artifacts
- Outputs: .pb.h, .arrow.pb.h, .accessor.pb.h, .ts, .rs, .ipc files
- Both C++ tests depend on generate_coverage_outputs
- Linked with fletcher-arrow-bridge, fletcher-pubsub

**Status:** Not a stub. Actual encode/decode round-trip + Arrow view + IPC validation + RBA reads. CONFORMS.

---

### 2. Graceful SKIP When rustc/tsc Absent

**run_tsc_check.cmake:**
- Guard: if(NOT TSC_EXECUTABLE OR NOT EXISTS)
- Action: prints SKIP_MARKER and returns (exit 0)
- Fallback: runs tsc --noEmit if present; fatal only on nonzero rc

**run_cargo_check.cmake:**
- Guard: checks both CARGO_EXECUTABLE and RUSTC_EXECUTABLE
- Action: prints SKIP_MARKER and returns (exit 0)
- Fallback: runs cargo check --locked --tests; fatal only on nonzero rc

**CMakeLists.txt:**
- find_program at configure time
- Tests added via add_test invoking cmake scripts
- set_tests_properties SKIP_REGULAR_EXPRESSION "SKIP_MARKER"

**Mechanism:** Tool absent => script prints marker => exit 0 => CTest sees marker => reports Skipped. Tool present but fails => rc != 0 => fatal error => Failed. Tool present and passes => no marker => Passed. No false-pass path.

**Status:** CONFORMS.

---

### 3. Cross-Platform Commands & CI Wiring

**ci.integration-test.protoc-coverage.yml:**
- Linux job: sparse-checkout, devcontainer pull, conan build with Linux-gcc13-x86_64-Release
- Windows job: sparse-checkout, conan create components, conan build with Windows-msvc194-x86_64-Release
- Both use conan build as generator-agnostic abstraction

**ci.pr.yml:**
- Adds integration-protoc-coverage output to detect-changes
- Path-filter: core/**, protoc/**, arrow-bridge/**, pubsub/**, integration-tests/protoc-coverage/**
- Calls reusable workflow with devcontainer-image input
- Added to final-result aggregation

**Status:** Reusable workflow + path-filter + devcontainer + sparse-checkout all present. CONFORMS.

---

### 4. Locked-Decision Conformance

**Decision #2 (Wire bytes):**
- ServiceRequest.row renamed to payload to avoid RBA accessor name collision
- Field number 1, type CompositeCoverage preserved
- Wire format unchanged (field metadata unchanged)
- Status: CONFORMS

**Decision #3 (RBA read-only):**
- Accessor test reads up to depth-2/3 cap
- No changes to recordbatch_accessor_emitter.cpp
- RBA linked but not modified
- Status: CONFORMS

**Decision #1 (Language-neutral IR):**
- Enum values are int32 literals, not C++ type strings
- No proto-to-IR table using C++ spelling
- Status: CONFORMS

**Decision #8 (Edge codec seams):**
- .pb.h (edge row), .arrow.pb.h (Arrow view), .accessor.pb.h (RBA) all generated
- --fletcher_opt=ipc,accessor,ts,rust does not foreclose either path
- IPC schema files available
- Status: CONFORMS

---

### 5. Deviations Within Risk Mechanisms

**Scalar-leaf flatten parked in coverage_future.proto:**
- coverage.proto documents why ScalarListWrapper and nesting are absent
- Field numbers 11, 13, 15, 17 left as gaps
- coverage_future.proto collects all variants with decision-rule record
- Not wired into CMakeLists.txt generation
- Status: Within design risk mechanism. CONFORMS.

**ServiceRequest.row to payload rename:**
- RBA accessor internally names its per-row index row
- Proto field row collides (member + getter both named row)
- Constraint: field #1, type CompositeCoverage, wire format unchanged
- Documented in proto comment
- Status: Wire-neutral, RBA unmodified. CONFORMS.

**Accessor test links fletcher-pubsub:**
- ToArrowRow includes <fletcher/pubsub/owned_schema.hpp> transitively
- CMakeLists.txt documents reason at lines 127-129
- Status: Design-anticipated. CONFORMS.

---

## Blocking Conformance Issues

None.

---

## Summary

All five pressure points verified:
1. Actual compile + execute: edge encode/decode, Arrow view, IPC readable, RBA accessor working
2. Graceful skip: SKIP_MARKER + SKIP_REGULAR_EXPRESSION correct
3. Cross-platform / CI: reusable workflow, path-filter, sparse-checkout, devcontainer
4. Locked decisions: wire bytes preserved, RBA read-only, no IR type strings, edge codec seams open
5. Deviations justified: coverage_future.proto rule applied, field rename wire-neutral, pubsub linkage explained

**Report:** plans/reviews/GIR-1-compliance.md  
**Date:** 2026-07-10
