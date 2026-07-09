# HARD — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[HARD-runtime-pubsub-hardening.md](HARD-runtime-pubsub-hardening.md) for the
tracker.

<!-- Entries appended below by the round runbook -->

## HARD-1 — `DecodeScalarFromReader` decode correctness (2026-07-09)

**Forcing test:** `FixedSizeBinaryOwnsBytesAfterSourceFreed` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/HARD-1-scalar-codec-decode.md`  ·  Step-2: APPROVE (cycle 2)
**What landed:** FIXED_SIZE_BINARY decode now copies into an owned buffer instead of wrapping the reader's transient pointer in a non-owning `arrow::Buffer` (was a dangling reference, #52); the string/binary block is self-terminating and the unreachable duplicate tail `throw` is removed (#58). Wire bytes unchanged (H-INV-1).
**Files touched:** `arrow-bridge/src/scalar_codec.cpp`, `arrow-bridge/tests/test_codec.cpp`, `arrow-bridge/tests/CMakeLists.txt`
**Reviews:** compliance `CONFORMS` · code-review `blocking 0 / should-fix 0 / nit 0`
**Verification:** inner-loop `PASS — 33/33 tests, arrow-bridge, no C4702/C4715`; full suite `deferred to HARD-3 checkpoint`
**Commit / push:** `hard/1-scalar-codec-decode` → `origin hard/1-scalar-codec-decode` (draft PR, base `main`)
**Carry-forwards / stop-and-asks:** none. #58 ruled EXEMPT from decision #9 (provably-unreachable dead-code removal; validated by unreachability + warnings-clean build). Test target gained `../src` on PRIVATE includes to reach the internal `detail::` API (test-only wiring).
