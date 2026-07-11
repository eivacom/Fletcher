# GIR-8 Conformance Review

**Target:** branch diff against d71952bb682d4a9aa31997a6a3416d68f38bfc32
**Verdict:** needs-attention
**Review Tool:** Codex Adversarial Review

## Summary

No-ship: the implementation mostly matches the generator behavior contract, but the GIR-8 zero-ValueOrDie forcing test is materially under-scoped and can pass while violating the design across other generated integration outputs.

## Findings

### [HIGH] NoValueOrDie check only scans protoc-coverage, not all IR-emitted integration outputs
- **Location:** integration-tests/protoc-coverage/CMakeLists.txt:285-288
- **Issue:** The design explicitly requires the zero-.ValueOrDie assertion to run over the full integration suite, including generated outputs from protoc-arrow-bridge, gateway, npm/cli, and TS harnesses, excluding only RBA/binary outputs. The wired test passes only `-DGENERATED_DIR=${GENERATED_DIR}` from `integration-tests/protoc-coverage`, and the script recursively scans only that one directory. Existing integration CMake files generate additional `.fletcher.pb.h`, `.fletcher.arrow.pb.h`, and `.fletcher.ts` outputs outside this directory, so a future or existing emitter path used by those harnesses can still emit `.ValueOrDie()` while `GenErrors.NoValueOrDieInIrGeneratedCode` stays green.
- **Impact:** Weakens the locked red-first test requirement and makes the safety contract non-exhaustive.
- **Recommendation:** Either wire this check at the top-level/full integration test layer with every non-RBA generated root, or add per-harness invocations for each integration directory that emits IR-generated `.fletcher.pb.h`, `.fletcher.arrow.pb.h`, or `.fletcher.ts` files. Keep the script's RBA and `.ipc` exclusions, but make the scanned roots match the design's full-suite scope.

## Next Steps

1. Expand the NoValueOrDie test coverage beyond `integration-tests/protoc-coverage/generated`.
2. Re-run the GIR-8 negative generation and zero-grep tests after widening the scanned roots.

---

## Targeted Re-Review: 4a + 4b Fix Verification (2026-07-11)

**Target:** Staged fixes for two GIR-8 findings (4a: full-surface ValueOrDie guard; 4b: repeated/map Any classification)
**Review Scope:** Confirm each fix resolves its finding and no regression was introduced
**Test Status:** GREEN (protoc 53/53 unit + 3 pkg; coverage 12/12 incl. 3 unsupported-fail tests + NoValueOrDie + all oracles byte-identical; RBA no-drift)

### 4a Re-Review: Full-Surface Zero-ValueOrDie Guard

**Finding Status:** RESOLVED

The prior HIGH finding about under-scoped test coverage has been addressed by a two-tier approach:

1. **Full-Surface Source Grep** (NEW): `GenErrors.NoValueOrDieInEmitterSources` (protoc/tests/CMakeLists.txt)
   - Greps ALL IR emitter TUs in `protoc/src` (*.cpp and *.hpp)
   - RBA (`recordbatch_accessor_emitter.*`) correctly excluded (read-only, reconciled separately)
   - Red-capable: FATAL_ERROR if any `.ValueOrDie(` pattern found
   - Scope: ALL emitters caught at once → invariant enforced for every downstream harness by construction
   - No emitter TU missed (verified: 13 emitter files in protoc/src, all matched by glob)

2. **Generated Output Spot-Check** (COMPLEMENTARY): `GenErrors.NoValueOrDieInIrGeneratedCode` (integration-tests/protoc-coverage)
   - Scans protoc-coverage's IR-emitted generated files (*.fletcher.pb.h / *.fletcher.arrow.pb.h / *.fletcher.ts)
   - RBA and binary .ipc correctly excluded
   - Proves the approach works on real generated output
   - Comment (CMakeLists.txt:272-275) explicitly documents: "The companion full-surface guard that greps the EMITTER SOURCES for every harness at once lives in the protoc unit suite"

**Implementation Verification:**
- All `.ValueOrDie()` calls in emitter code (cpp_backend_view_visitor.cpp, generator.cpp) have been replaced with `detail::FletcherValueOrThrow()`
- Coverage-generated-output grep retained (not removed by 4a changes)
- New `FletcherValueOrThrow()` helper in generated view headers throws descriptive std::runtime_error instead of aborting
- Source grep test: confirmed zero `.ValueOrDie(` matches in protoc/src (excluding RBA)

### 4b Re-Review: Repeated/Map Any Root Fix

**Finding Status:** RESOLVED

The root cause of repeated/map Any being silently skipped (missing error) has been fixed in ir.cpp.

**DynamicWktUnsupportedReason() Helper:**
- Scoped precisely to `google.protobuf.Any` and `google.protobuf.Struct` only
- Does NOT reclassify other WKTs (Timestamp, Duration, wrappers remain supported)
- Applied identically in three contexts: BuildSingularMessage (line 440), BuildRepeatedMessage (line 352), BuildMapNode (line 415)
- Returns `std::optional<std::string>` with UNSUPPORTED reason or nullopt for mappable messages

**Regression-Free Verification:**
- Coverage.proto audit: no repeated/map Any or Struct fields found → no existing test reclassified
- Three new fixtures isolated from coverage generation set:
  - `coverage_unsupported.proto`: singular Any (field "payload")
  - `coverage_unsupported_repeated.proto`: repeated Any (field "payloads")
  - `coverage_unsupported_map.proto`: map<string, Any> (field "payload_map")
- Each fixture + test pair validates one shape in isolation (error must name that shape's field)
- All three tests validate error contains reason "google.protobuf.Any is dynamically typed"
- Oracles byte-identical (suite GREEN)

**Recursion/Safety Verified:**
- ValidateNoUnsupportedIr() skips recursive messages and flattened wrappers (same predicates as emit loops)
- Recursion stays non-fatal exactly as before (skipped, not classified as UNSUPPORTED)
- No wire/schema/TS/view change for supported types
- RBA untouched
- FletcherValueOrThrow() is generated-code helper, adds no single-language strings to IR nodes

### Verdict

**APPROVE: Both fixes conform to design and resolve their findings. No regression detected.**

- 4a: Strengthened via source-level full-surface grep; generated-output spot-check retained
- 4b: Root fix correctly classifies repeated/map Any/Struct; isolated new tests; regression-free
- All suite tests passing; all oracles byte-identical
