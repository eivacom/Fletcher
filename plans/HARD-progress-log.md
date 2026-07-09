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

## HARD-2 — Checked Arrow `Result<T>` access (runtime) (2026-07-09)

**Forcing test:** `CodecTest.BadResultThrowsInsteadOfAbort` (+ `CodecMapEncodeBadResultThrowsInsteadOfAbort`) → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/HARD-2-checked-arrow-result.md`  ·  Step-2: APPROVE (cycle 2)
**What landed:** New `fletcher::detail::ValueOrThrow<T>` helper (`arrow-bridge/include/fletcher/arrow_bridge/detail/arrow_result.hpp`; ok-check then `std::move(r).ValueUnsafe()`, throws `std::invalid_argument` with the op name + `status().ToString()`). All 13 hand-written `.ValueOrDie()` sites replaced — 9 in `codec.cpp`, 4 in the public `arrow_row_view.hpp`. Runtime aborts on a bad Arrow `Result<T>` are now catchable exceptions. The generator's 25 EMITTED `.ValueOrDie()` sites are untouched (GEN round, H-INV-5).
**Files touched:** `arrow-bridge/include/fletcher/arrow_bridge/detail/arrow_result.hpp` (new), `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp`, `arrow-bridge/src/codec.cpp`, `arrow-bridge/tests/test_codec.cpp`
**Reviews:** compliance `CONFORMS` · code-review `blocking 0 / should-fix 0 / nit 0`
**Verification:** inner-loop `PASS — 35/35 tests, arrow-bridge`; full suite `deferred to HARD-3 checkpoint`
**Commit / push:** `hard/2-checked-arrow-result` → `origin hard/2-checked-arrow-result` (draft PR, base `hard/1-scalar-codec-decode`)
**Carry-forwards / stop-and-asks:** none. Codec-path forcing test drives a real map-key `GetScalar` site — the design's `MakeBuilder`/`Finish` examples are not reliably inducible in Arrow 23, and the design said "for example". New public detail header ships via `conanfile.py`'s `*.hpp` wildcard (no CMake edit).

## HARD-3 — Surface discarded / swallowed errors (2026-07-09)

**Forcing test:** `OwnedSchemaTest.DeepCopyFailureThrows` (#54) + `FletcherTopicTypeTest.SerializeCapturesEncoderExceptionDiagnostic` (#60) → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/HARD-3-surface-errors.md`  ·  Step-2: APPROVE (cycle 2)
**What landed:** #54 — `OwnedSchema::DeepCopy` now captures the `ArrowSchemaDeepCopy` return code and throws `std::runtime_error` on failure (was silently discarded → empty schema); fixed in place (decision #8), `#include <string>` added. #60 — extracted `TransportData`+`FletcherTopicType` into `fastdds-pubsub-provider/src/internal/fletcher_topic_type.hpp` (verbatim, byte-identical happy path) to create a test seam; `serialize`'s `catch(...)` at :177 now captures the cause via a `noexcept SetLastSerializeError(const char*)` (inner try/catch, no alloc) before returning false — no exception escapes the DDS callback (H-INV-3). The unrelated `catch(...)` at :333 is untouched.
**Files touched:** `pubsub/include/fletcher/pubsub/owned_schema.hpp`, `pubsub/tests/test_owned_schema.cpp` (new), `pubsub/tests/CMakeLists.txt`, `fastdds-pubsub-provider/src/internal/fletcher_topic_type.hpp` (new), `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp`, `fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp`, `fastdds-pubsub-provider/tests/CMakeLists.txt`
**Reviews:** compliance `CONFORMS` · code-review `blocking 0 / should-fix 0 / nit 1` (Low: `SetLastSerializeError` comment slightly misleading re: which mutex guards it — synchronization is correct; logged, not gated)
**Verification:** inner-loop `PASS — pubsub 17/17, fastdds 17/17`; **FULL-SUITE CHECKPOINT 🟢** — all 6 components (core, arrow-bridge, pubsub, pubsub-arrow, fastdds, xrce) built + test_package green; integration `protoc-arrow-bridge` **72/72 passed** (1 skipped: `AccessorTest.GeneratedRustFileParsesWithRustc`, no rustc on this box). No regression from HARD-1/2/3.
**Commit / push:** `hard/3-surface-errors` → `origin hard/3-surface-errors` (draft PR, base `hard/2-checked-arrow-result`)
**Carry-forwards / stop-and-asks:** none. Checkpoint note: the round's `full_suite_cmd` used the wrong configure preset for the VS multi-config generator (`cmake --preset conan-release`); the configure preset is `conan-default`, build/test presets are `conan-release`. Fixed in `.claude/runbook.HARD.config.md` for the HARD-6 / close checkpoints. `$PROFILE` must be an absolute path (the loop `cd`s into each component dir).

## HARD-4 — Provider lifetime & callback re-entrancy (2026-07-09)

**Forcing test:** `ReentrantUnsubscribeNoUseAfterFree` (xrce) + `DestructAfterQuiescentUseDocumentsContract` (fastdds) → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/HARD-4-provider-lifetime.md`  ·  Step-2: APPROVE (cycle 3)
**What landed:** #62-residual — XRCE `OnTopic` now snapshots `callback`/`shared_schema`/`pending` into locals before invoking the user callback (both schema-flush and data paths), so a re-entrant `Unsubscribe` (which does an **in-place reset** `callback=nullptr`+`pending.clear()`, not a map erase) can no longer self-destruct the executing `std::function` or invalidate the pending iteration. Dispatch stays under the recursive mutex (decision #10); no blanket try/catch. #63 — FastDDS destructor gets a documented "no concurrent calls / no re-entrant callback in flight during destruction" quiescence precondition (comment on `.cpp` + doc comment on the header declaration); **no** destructor lock (non-recursive mutex → deadlock-unsafe; decision #4).
**Files touched:** `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`, `xrcedds-pubsub-provider/src/internal/xrce_test_hook.hpp` (new, non-installed, `FLETCHER_BUILD_TESTS`-guarded), `xrcedds-pubsub-provider/tests/test_xrce_provider.cpp`, `xrcedds-pubsub-provider/CMakeLists.txt` + `tests/CMakeLists.txt`, `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp`, `fastdds-pubsub-provider/include/fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp` (doc comment), `fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp`. Also corrected the spec's imprecise #62 "erases" prose in `docs/runtime-hardening-spec.md`.
**Reviews:** compliance `CONFORMS` · code-review `blocking 0 / should-fix 0 / nit 0`
**Verification:** inner-loop `PASS — xrce 10/10, fastdds 18/18`; full suite `deferred to HARD-6 checkpoint`. Red-for-the-right-reason: reverting the `OnTopic` fix makes `ReentrantUnsubscribeNoUseAfterFree` crash with heap corruption (0xC0000374) — the UAF — deterministic in Release, no sanitizer; post-fix `EXPECT_NO_THROW` + `delivery_count==2`.
**Commit / push:** `hard/4-provider-lifetime` → `origin hard/4-provider-lifetime` (draft PR, base `hard/3-surface-errors`)
**Carry-forwards / stop-and-asks:** none. Two design deviations accepted (both within the design contract): pre-fix manifests as a heap-corruption crash rather than the predicted catchable `bad_function_call` (stronger form of the same UB); the test seam is a `FLETCHER_BUILD_TESTS`-guarded static-init trampoline (a free hook fn cannot name the private pimpl `Impl` — C2248) driving the real `OnTopic`, production build unaffected, public header untouched.

## HARD-5 — Document last-callback-after-Unsubscribe (2026-07-09)

**Forcing test:** *(docs-only — review-verified, not test-gated)* → 🟢
**Design:** spec §HARD-5 (no separate design doc — one doc comment)
**What landed:** Extended the `Subscriber::Unsubscribe` doc comment (`pubsub/include/fletcher/pubsub/subscriber.hpp`) to document the intentional "one final in-flight message after Unsubscribe" behaviour and why: the fan-out (`subscriber.cpp:52-74`) snapshots `(id, callback)` under `mu`, releases the lock, then invokes outside it, so an unsubscribe landing in that window still gets one final delivery. Framed as by-design, not a bug. No code/behaviour change.
**Files touched:** `pubsub/include/fletcher/pubsub/subscriber.hpp` (comment only)
**Reviews:** accuracy review `ACCURATE` (verified against the fan-out code) · docs-only confirmed
**Verification:** none (docs-only); no build/test run
**Commit / push:** `hard/5-doc-unsubscribe` → `origin hard/5-doc-unsubscribe` (draft PR, base `hard/4-provider-lifetime`)
**Carry-forwards / stop-and-asks:** none.
