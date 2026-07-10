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
