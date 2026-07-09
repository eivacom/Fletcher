# HARD — Runtime & PubSub Hardening — Execution Plan

Round plan + tracker for the runtime/pubsub correctness round.
Spec: [docs/runtime-hardening-spec.md](../docs/runtime-hardening-spec.md).
Locked decisions: [HARD-locked-decisions.md](HARD-locked-decisions.md).
This file is both the `plan_path` (the tracker) and the `user_stories_path`.

## Goal

Close the runtime/pubsub correctness issues migrated from the README TODO list
(and one residual found during triage): no dangling references, no
abort-on-error, no silently-swallowed/discarded failures, no callback
use-after-free, plus `[[nodiscard]]` + helper-dedup hygiene. **Wire format
byte-identical; generator untouched** (hard invariants H-INV-1 / H-INV-5).

## Branch strategy

- **Base on `main`.** This round touches only arrow-bridge, core, pubsub, and
  the two providers — none of the protoc/version-bump work on
  `chore/protoc-0.4.1-alpha`. Confirm the base at kickoff (sets
  `branch`/`push_targets` in the config). Suggested branch:
  `feature/runtime-hardening`.
- Rebased onto `main`, not merged (repo convention). No PR until the round is
  green and reviewed; the PR/merge is the user's step.
- **Independent of `feature/robustness_improvements`.** That stale branch (10
  behind `main`, unmerged) covers a *different* class — reader-bounds hardening
  against untrusted wire bytes. This round neither depends on nor duplicates it.

## Sequencing

Strictly linear; each item's forcing test must be 🟢 before the next starts:

```
HARD-1  scalar_codec decode      →  HARD-2  checked Arrow Result (runtime)  →
HARD-3  surface silent errors    →  HARD-4  provider lifetime / re-entrancy →
HARD-5  docs (Unsubscribe)       →  HARD-6  [[nodiscard]]                   →
HARD-7  consolidate helpers
```

---

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)

| Item | Title | Issues | Forcing test | Status |
|------|-------|--------|--------------|--------|
| HARD-1 | `DecodeScalarFromReader` decode correctness | #52, #58 | `FixedSizeBinaryOwnsBytesAfterSourceFreed` | 🟢 |
| HARD-2 | Checked Arrow `Result<T>` access (runtime) | #53 (rt) | `CodecTest.BadResultThrowsInsteadOfAbort` | 🟢 |
| HARD-3 | Surface discarded / swallowed errors | #54, #60 | `OwnedSchemaTest.DeepCopyFailureThrows` (+ serialize diag) | 🟢 |
| HARD-4 | Provider lifetime & callback re-entrancy | #63, #62-res | `ReentrantUnsubscribeNoUseAfterFree` | 🟢 |
| HARD-5 | Document last-callback-after-Unsubscribe | #65 | *(docs — review-verified, not test-gated)* | 🟢 |
| HARD-6 | `[[nodiscard]]` on public API | #56 | `NodiscardTest` (compile-fails-on-discard TU) | ⚪ |
| HARD-7 | Consolidate duplicated helpers | #57 | existing suites green + single-definition check | ⚪ |

Suite shape: reuses each component's existing test target
(`arrow-bridge/tests/test_codec.cpp`, `pubsub/tests/test_publisher_subscriber.cpp`,
`fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp`,
`xrcedds-pubsub-provider/tests/test_xrce_provider.cpp`,
`core/tests/test_envelope.cpp` / `test_positional_io.cpp`). New TUs only where a
component has no natural home for the forcing test (e.g. a `test_scalar_codec.cpp`
for HARD-1 if `test_codec.cpp` doesn't already exercise `DecodeScalarFromReader`,
and a compile-failure TU for HARD-6) — confirm at design time.

---

## Items (user stories + acceptance)

### HARD-1 — `DecodeScalarFromReader` decode correctness

**Story.** As a consumer decoding wire bytes into Arrow scalars, a decoded
`FixedSizeBinary` value stays valid after the source buffer is freed, and an
unsupported type is a clean throw — not a dangling read or a fall-through into
dead code.

**Scope.** `arrow-bridge/src/scalar_codec.cpp`, `DecodeScalarFromReader` only:
- **#52:** `FIXED_SIZE_BINARY` (`:272-278`) — replace the non-owning
  `std::make_shared<arrow::Buffer>(ptr, byte_width)` (`:276`) with an **owned
  copy**, mirroring the string branch's `arrow::Buffer::FromString(...)`
  (`:231-232`).
- **#58:** the string/binary block (`:223-250`) must not `break`-fall-through to
  the **unreachable** duplicate `throw` at `:317`; make the block
  self-terminating and delete the `:317` throw, keeping the reachable `default`
  throw (`:313-315`). Function must not fall off the end.

**Forcing test** (`arrow-bridge/tests`): decode a `FixedSizeBinary(n)` scalar
from a heap buffer, then free/overwrite that buffer, and assert the scalar still
returns the original bytes (red before the fix: dangling read). Plus a
round-trip over every string/binary/fixed-size-binary variant, and an
unsupported-type case that throws `std::invalid_argument` (not abort).

**Acceptance.** Spec HARD-1 acceptance. No wire-byte change; existing codec
round-trip tests stay green.

### HARD-2 — Checked Arrow `Result<T>` access (runtime half of #53)

**Story.** A runtime path handed an error `Result<T>` throws a catchable
exception instead of aborting the process.

**Scope.**
- `arrow-bridge/src/codec.cpp` — replace 9 `.ValueOrDie()` (lines 96, 139, 153,
  225, 238, 281, 291, 306, 307) with a checked access that throws
  `std::invalid_argument` naming the operation.
- `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp` — same for 4
  sites (lines 74, 108, 145, 238).
- Prefer one small `ValueOrThrow(...)` helper over 13 ad-hoc guards.
- **Not** the 25 generator-emitted sites (H-INV-5 / decision #7).

**Forcing test** (`arrow-bridge/tests`): drive at least one runtime `Result`
path to failure (e.g. a builder/finish or `GetScalar` on an out-of-range index)
and assert it throws rather than aborts (red before: process abort).

**Acceptance.** Spec HARD-2 acceptance.

### HARD-3 — Surface discarded / swallowed errors

**Story.** A failed schema deep-copy and a failed serialize each leave a
diagnostic instead of a silent empty/false.

**Scope.**
- **#54:** `pubsub/include/fletcher/pubsub/owned_schema.hpp:53-57` — capture the
  `ArrowSchemaDeepCopy` return and throw on non-zero. **In place** (decision #8).
- **#60:** `fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp:177-180` —
  add `catch (const std::exception& e)` capturing `e.what()` into a
  logged/stored diagnostic before returning `false`; keep the catch-all. **No
  rethrow** (decision #3 / H-INV-3).

**Forcing test:** `OwnedSchemaTest.DeepCopyFailureThrows` — a `DeepCopy` whose
underlying copy fails throws with a message (red before: silent empty schema). A
serialize-failure test asserts the diagnostic is captured and `false` is still
returned without propagation.

**Acceptance.** Spec HARD-3 acceptance.

### HARD-4 — Provider lifetime & callback re-entrancy

**Story.** Destroying a FastDDS provider has a clear, documented safety
contract; and an XRCE subscriber whose callback unsubscribes its own topic
mid-delivery does not corrupt memory.

**Scope.**
- **#63:** `fast_dds_pubsub_provider.cpp:441-460` — document the "no concurrent
  API calls during destruction" precondition on the destructor; add a defensive
  `lock_guard(impl_->mu)` **iff proven deadlock-free** (decision #4).
- **#62 residual:** `xrce_dds_pubsub_provider.cpp` `OnTopic` — do not hold
  `auto& ts` (`:216`) across the callback (`:235`; schema-flush `:201-207` then
  `ts.pending` at `:207`). Copy the fields needed after the callback (or restructure)
  so a re-entrant `Unsubscribe`/erase is safe. Narrow point-fix (decision #10).

**Forcing test** (`xrcedds-pubsub-provider/tests`):
`ReentrantUnsubscribeNoUseAfterFree` — deliver to a subscriber whose callback
calls `Unsubscribe` on its own topic; the provider completes delivery without
UAF (red before under ASan/logic: dangling `ts` access). A FastDDS destruction
test exercises the documented precondition.

**Acceptance.** Spec HARD-4 acceptance. (Run the XRCE test under ASan if the
harness supports it, to make the UAF regression detectable.)

### HARD-5 — Document last-callback-after-Unsubscribe (#65)

**Story.** A reader of `Subscriber::Unsubscribe` learns that one final message
may arrive after unsubscribing, and why.

**Scope.** `pubsub/include/fletcher/pubsub/subscriber.hpp:60-62` — extend the
`Unsubscribe` doc comment to state the copy-then-release-then-call fan-out
(`subscriber.cpp:52-74`) and the resulting one-final-message guarantee. **Docs
only, no code change.**

**Acceptance.** Doc comment present and accurate; verified by review (not
test-gated — decision #9).

### HARD-6 — `[[nodiscard]]` on public API (#56)

**Story.** Silently discarding these meaningful return values is a compile-time
error.

**Scope.** Add `[[nodiscard]]` to the nine declarations in the spec HARD-6
table (`positional_io.hpp:267`, `subscriber.hpp:58`, `provider.hpp:103`,
`publisher.hpp:42`, `codec.hpp:48/50/51`, `envelope.hpp:39/74/117`,
`owned_schema.hpp:53`). Additive only (decision #5). Sequenced after HARD-3
(both touch `owned_schema.hpp` `DeepCopy`).

**Forcing test:** a translation unit that discards each annotated return value
fails to compile under `-Werror` / `[[nodiscard]]` diagnostics (compile-failure
harness — confirm the mechanism the suite supports at design time). All
components still build.

**Acceptance.** Spec HARD-6 acceptance.

### HARD-7 — Consolidate duplicated helpers (#57)

**Story.** `JoinSegments`, `AppendFixed`, and `BitfieldBytes` each have a single
source of truth; no behaviour or output changes.

**Scope.**
- `JoinSegments`: delete the copies in `xrce_dds_pubsub_provider.cpp:55-63`,
  `pubsub-arrow/src/publisher_arrow.cpp:80-90`, `subscriber_arrow.cpp:444`, and
  the mocks (`test_publisher_subscriber.cpp:75`, `test_pubsub_arrow.cpp:76`,
  `gateway/src/main.cpp:127`); include the existing
  `pubsub/include/fletcher/pubsub/internal/segments.hpp`.
- `AppendFixed<T>`: single arrow-bridge `detail` definition
  (`codec.cpp:26-29` + `scalar_codec.cpp:18-22` → one header).
- `BitfieldBytes`: one `core` definition (collapse `positional_io.hpp:140`/`:418`);
  the `codec.cpp:34` caller keeps its `int64_t` overflow-safe intent (decision #6).

**Forcing test:** the full component + integration suites stay green after
consolidation; a grep/build check confirms each helper has exactly one
definition. No wire/generated output changes (byte-identity assertions green).

**Acceptance.** Spec HARD-7 acceptance.

---

## Definition of done (round)

All forcing tests 🟢 (HARD-5 review-verified); the full component suites
(`core`, `arrow-bridge`, `pubsub`, `pubsub-arrow`, `fastdds-pubsub-provider`,
`xrcedds-pubsub-provider`) and the integration suite green; wire format
byte-identical (no regression in round-trip/byte-identity assertions); no
generator or generated-code change; each behavioural fix carries a red-first
negative test; docs updated (HARD-5); nothing in the spec's out-of-scope list
touched. On completion, verify the corresponding issues (#52, #53, #54, #56,
#57, #58, #60, #63, #65) can be closed and note the #62 residual is resolved.
