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
