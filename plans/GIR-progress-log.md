# GIR — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[GIR-generator-ir-rewrite.md](GIR-generator-ir-rewrite.md) for the tracker.

<!-- Entries appended below by the round runbook -->

## GIR-1 — Phase 3a: generator compile-and-run harness (2026-07-10)

**Forcing test:** `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/GIR-1-compile-and-run-harness.md`  ·  Step-2: APPROVE (1 rework cycle — Linux preset, graceful-skip, CI wiring)
**What landed:** A greenfield `integration-tests/protoc-coverage/` harness that runs `protoc-gen-fletcher` on a broad `coverage.proto` (one `--fletcher_opt=ipc,accessor,ts,rust` invocation) and actually **compiles + executes** the generated surfaces — C++ edge encode/decode, Arrow view + `ToArrowRow`, IPC-schema readability, RBA C++ accessor (read-only) — building a row and reconstructing it with value-equality. TS/Rust compile-checks report **Skipped** when the toolchain is absent. This is the guard GIR-2's oracles and the GIR-3..7 migrations lean on.
**Files touched:** `integration-tests/protoc-coverage/**` (`coverage.proto`, `coverage_future.proto`, `tests/{test_coverage_harness,test_coverage_accessor}.cpp`, `coverage_fixture.hpp`, `ts/**`, `rust-accessor/**`, `cmake/{run_tsc_check,run_cargo_check,validate_generated_ipc}.cmake`, `CMakeLists.txt`, `conanfile.py`, `README`); `.github/workflows/ci.integration-test.protoc-coverage.yml` (new) + `.github/workflows/ci.pr.yml` (caller: path filter + devcontainer-image + sparse-checkout). No production generator/component source touched.
**Reviews:** compliance `CONFORMS` (0 blocking) · code-review `blocking 4→0 (resolved+re-verified) / non-blocking 3 (1 folded, 2 deferred) / nit 3`  (full: `plans/reviews/GIR-1-conformance.md`, `plans/reviews/GIR-1-codereview.md`, `plans/reviews/GIR-1-codereview-fix-verification.md`)
**Verification:** inner-loop `PASS` (coverage ctest 4/4) · full suite `PASS @ GIR-1 checkpoint` — all 7 components + protoc-arrow-bridge + protoc-coverage harnesses + Rust crate green, zero build errors  (accepted residuals: `AccessorTest.GeneratedRustFileParsesWithRustc` Skipped, `CoverageHarness.GeneratedTypescriptCompiles` Skipped — tsc/rustc absent on box)
**Commit / push:** `feature/generator-ir-rewrite` → `origin feature/generator-ir-rewrite`
**Carry-forwards / stop-and-asks:**
- **Latent RBA fragility (for RIR):** a proto field literally named `row` collides with the RBA accessor's internal `row` index → generated accessor won't compile. Worked around fixture-only (`ServiceRequest.row`→`payload`; field number/type unchanged, wire-neutral; RBA emitter left read-only per decision #3). Flag for the RBA↔IR reconciliation round.
- **`coverage_future.proto`** parks the scalar-leaf nested-list / flatten family the current generator can't faithfully emit (non-compiling / silently-collapsing / TS-syntax-error cases); GIR-10 enables them.
- **Deferred non-blocking review items:** #5 (function-local `static` in `test_coverage_harness.cpp`), #6 (`cargo --locked` — already honored). One message-clarity nit in `validate_generated_ipc.cmake`.
- **CI:** protoc-coverage is the first integration-test Windows job to use an **absolute** Conan-profile path (more robust); siblings still use relative — consider propagating (out of GIR-1 scope).

## GIR-2 — Phase 3b: encode byte-identity + decode round-trip oracles (2026-07-10)

**Forcing test:** `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/GIR-2-parity-oracles.md`  ·  Step-2: APPROVE (1 rework cycle — golden-*encode* anchoring, non-sorted maps, both-state nullables)
**What landed:** A parity-oracle TU (`test_parity_oracle.cpp`) in the GIR-1 harness that PINS the generated wire contract across 12 fixtures: per fixture it asserts `Encode() == golden` AND `EncodeRow(ToArrowRow()) == golden` AND `Encode()==EncodeRow()` (so a *coordinated* encode drift is caught), plus encode→decode→value-equality and decode-of-known-golden reading back map keys/values + struct inner fields. This is the standing guard every GIR-3..7 emitter migration must keep green. **The oracle immediately caught a real memory-corruption bug** and its fix was folded in (below).
**Wire-corruption fix folded in (locked decision #6):** `protoc/src/generator.cpp:1590` — the nullable-`bytes` `WriteBinary` branch emitted a stray `*` (`reinterpret_cast<const uint8_t*>(*x->data())`), dereferencing the `const char*` to a byte then reinterpreting that value as a pointer → garbage reads / SEH crash for any SET optional/wrapped `bytes` field. `Encode()` was corrupt while runtime `EncodeRow()` was correct — a shipping `Encode()≠EncodeRow()` wire defect. Removed the `*` (matches the non-nullable branch); no sibling antipattern found. Goldens baselined against the CORRECTED `Encode()`. **Surfaced as a stop-and-ask; maintainer authorized the fix.**
**Files touched:** `protoc/src/generator.cpp` (1-site fix); `integration-tests/protoc-coverage/tests/{test_parity_oracle.cpp (new), coverage_test_helpers.hpp (new), coverage_fixture.hpp (extended)}`; `integration-tests/protoc-coverage/CMakeLists.txt` (new `coverage_parity_oracle_tests` target); `integration-tests/protoc-coverage/golden/` (README + 12 tracked `.bin`). RBA emitter untouched.
**Reviews:** compliance `CONFORMS` (0 blocking) · code-review `blocking 0 / non-blocking 0 / nit 0`  (full: `plans/reviews/GIR-2-conformance.md`, `plans/reviews/GIR-2-codereview.md`)
**Verification:** inner-loop `PASS` (protoc unit suite 45/45 post-fix; coverage ctest 6/6) · full suite `PASS @ GIR-2 checkpoint` — all 7 components + protoc-arrow-bridge (72/72) + protoc-coverage (6/6) + Rust crate green against the fixed generator, zero errors  (accepted residuals: `GeneratedRustFileParsesWithRustc` + `GeneratedTypescriptCompiles` Skipped (rustc/tsc absent); `ParityOracle.RegenerateGoldens` gated-Skip)
**Commit / push:** `feature/generator-ir-rewrite` → `origin feature/generator-ir-rewrite`
**Carry-forwards / stop-and-asks:**
- **Golden re-baseline discipline (for GIR-3..7):** goldens are the wire contract. A migration causing golden churn is a wire change → re-baseline only under review via the gated `FLETCHER_REGEN_PARITY_GOLDENS=1` `RegenerateGoldens` path; a byte change for a previously-supported input is a stop-and-ask (decision #2).
- **`MakeScalarsAllSet()`** leaves `optional_int32`/`wrapped_int32` unset (base sets them) — both presence axes covered per field (re-review-blessed); minor: those two fields' unset path is covered only by-class.
- The nullable-`bytes` fix is a genuine bug in shipping `main`; note for release notes / any downstream that encoded optional `bytes` via the edge path (it would have crashed, so unlikely relied upon).
