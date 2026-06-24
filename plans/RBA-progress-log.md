# RBA — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[RBA-recordbatch-accessor.md](RBA-recordbatch-accessor.md) for the tracker.

<!-- Entries appended below by the round runbook -->

## RBA-1 — Option plumbing + additive no-drift guarantee (2026-06-24)

**Forcing test:** `AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/RBA-1-opt-plumbing-no-drift.md`  ·  Step-2: APPROVE (after 1 rework cycle — 4 items resolved)
**What landed:** Two default-off `--fletcher_opt` tokens (`accessor`, `rust`) that each emit one new output file (`<stem>.fletcher.accessor.pb.h`, `<stem>.fletcher.rs`) via **new** emitter entry points, unconditionally (one file per token, never content-gated). No existing emitter function touched and no existing output byte changed (D-RBA-1). RBA-1 files are minimal-but-valid skeletons; real accessor content lands RBA-2+.
**Files touched:** `protoc/src/generator.cpp` (opt parse + 2 additive `context->Open` blocks + 2 filename helpers), `protoc/src/recordbatch_accessor_emitter.{hpp,cpp}` (new), `protoc/CMakeLists.txt`, `integration-tests/protoc-arrow-bridge/CMakeLists.txt` (new `accessor_tests` target, 3 `-I` roots), `integration-tests/protoc-arrow-bridge/tests/test_accessor.cpp` (new), `integration-tests/protoc-arrow-bridge/proto/empty_accessor.proto` (new degenerate fixture).
**Reviews:** compliance `CONFORMANT` (initial HIGH on Rust well-formedness silent downgrade → fixed) · code-review `blocking 0 (2 resolved: staging + rustc-proof) / should-fix 0 (majors adjudicated) / nits logged`  (full: `plans/reviews/RBA-1-conformance.md`, `plans/reviews/RBA-1-codereview.md`)
**Verification:** inner-loop `PASS` — plugin unit suite 3/3; integration `ctest` 64/64 (62 pre-existing + 2 new Accessor); `rustc` parse-check RAN on this box (rustc 1.96.0, not skipped). Full round-wide suite (core unit tests + Rust crate) deferred to a later stage-close — the Rust crate does not exist until RBA-5. (accepted residuals: none)
**Commit / push:** `feature/recordbatchaccessor` → `origin feature/recordbatchaccessor`
**Carry-forwards / stop-and-asks:** none. (Won't-fix nits logged: unknown-token errors out of scope — acceptance = unknown ⇒ no new files; `FieldDescriptor*` null guards unneeded in plugin context; docs deferred to RBA-7.)
