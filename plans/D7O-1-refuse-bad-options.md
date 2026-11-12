# D7O-1 — Nonsensical options are refused (design)

Round D7O, domain 7, branch `feature/d7o-batch-options`, tree `8e25cca`. Story: `plans/D7O-plan.md`
§ D7O-1. Ladder: `plans/D7O-test-ladder.md` rung 1. Locked: D-D7O-3, D-D7O-4, D-D7O-6 (D-D7O-1
constrains only what stage 2 extends). Rulings ledger read in full: 285 lines, 23 entries, last
heading "2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows".
Citations: `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`, `hpp` = its header, `test` =
`pubsub-arrow/tests/test_pubsub_arrow.cpp`, `d7` = `docs/domains/d7-batch-assembly.md`.

## Summary

A batched `Subscribe` with `max_rows < 1` or a negative timeout throws
`PubSubError(kInvalidArgument)` before any batch thread, provider subscription or delivery exists.
The silent clamp (`cpp:36`) is deleted, and the refusal takes its place, so domain 7 answers "are these
options acceptable?" in one place (L15, BC-8). Every in-range value goes to the batcher unchanged.
No public type, method or enum changes. Only the header and README text change.

## Design

**The one check, and where it lives.** `RecordBatchBatcher`'s constructor (`cpp:33-38`) is the only
place a batch row count or timeout enters domain 7. It is reached only through the batched
`Subscribe` (`cpp:371`), and only before `Start` (`cpp:373`) and before `subscriber_->Subscribe`
(`cpp:383`). That constructor already holds the clamp `max_rows < 1 ? 1 : max_rows`. The clamp
becomes the refusal:

```cpp
max_rows_(max_rows), timeout_(timeout), ...
{
    // L15 / BC-8: the one answer to "are these options acceptable?". Refused, never adjusted.
    if (max_rows_ < 1 || timeout_ < std::chrono::milliseconds::zero())
        throw PubSubError(PubSubStatus::kInvalidArgument, "<names max_rows >= 1, timeout >= 0, and the values given>");
}
```

There is one predicate and one throw. When both fields are out of range, the caller gets one error.
The message names both rules and both values. Tests assert only the status, never the text.

**Why the constructor, not the first line of `Subscribe`** (the ladder's wording; L15 allows either).
It is the forbid-first rung 1 of the ladder: no batcher can exist with options out of range, so
`AddRow`'s `num_rows() >= max_rows_` (`cpp:108`), `Reserve(max_rows_)` (`cpp:72`, `cpp:218`) and
`ArmTimer`'s `last_flush_ + timeout_` (`cpp:187`) all rely on an invariant the type sets up for
itself. It also covers the class of problem, not just today's caller. A second batched entry point
(RIX stage S4 plans `SubscribeIndexed`) must build a batcher, so it gets the check for free and never
writes a second one (D-D7O-4; the stop-and-ask list's "second range check"). L15 is still met word
for word: the throw comes out of the batched `Subscribe` call, before anything is subscribed. The
register's BC-8 single-site text is updated to name the site exactly (see Files-to-touch).

**What a refused call leaves behind: nothing, by construction.**
- *No thread.* `Start` is never reached. A constructor that throws does not run the destructor, so
  `Stop` is not called either. `ended_` stays at its default.
- *No provider subscription, no delivery.* `subscriber_->Subscribe` is never reached. No entry is
  added to `batchers_`, `sub_topic_` or `codecs_`, because `AcquireCodec` runs only inside the row
  lambda.
- *The callback is released before the throw reaches the caller.* `make_shared` forwards
  `std::move(callback)` into the constructor's by-value parameter `cb`. `cb_` is move-constructed
  from it, and the exception destroys both `cb_` (a fully constructed member) and the parameter. The
  caller's `weak_ptr` to anything the callback captured has therefore expired when it reaches
  `catch`, with no polling needed. `StopOnThrow` (`cpp:377-382`) is declared after construction,
  so it plays no part.
- *Concurrency.* The check reads only its arguments. It takes no lock and touches no shared state,
  so a refusal cannot race with anything.

**In-range values are used as given.** This stage adds no code for them. `max_rows = 1` closes a
window on every row (`cpp:108`). A zero timeout gives `deadline = max(now + 1 ms, last_flush + 0)`
(`cpp:187`), so the 1 ms floor applies as L5 states. Huge values (`INT64_MAX`, `milliseconds::max()`)
pass this check, as L15 requires. What they *mean* is what stages D7O-2 and D7O-3 build. This stage
neither refuses them nor changes their behaviour.

**What stage 2 can rely on (D-D7O-1).** The predicate is `timeout < 0`. Whatever non-negative
spelling, or separate field, stage 2 chooses for "no timeout" passes it unchanged. Stage 2 *extends*
this check (for example, to read a new field) and never replaces it. The ladder already forbids a
negative spelling. This design adds nothing that stage 2 has to undo.

**Docs (not counted in net lines).** Audit findings F3–F5 are rewritten for the refusal only. The
no-timeout text and F2 belong to stage 2.
- `hpp:77-79`: the `BatchOptions` member comments state the ranges. `max_rows` is at least 1, and
  below 1 is refused. `timeout` is 0 or more, and negative is refused. A zero timeout delivers no
  sooner than 1 ms after the window's first row or drop.
- `hpp:93-103`: the batched `Subscribe` doc adds: "Throws `PubSubError(kInvalidArgument)` when
  `options.max_rows < 1` or `options.timeout` is negative, before anything is subscribed: no
  thread is started, the callback is released, and nothing is ever delivered. Any other value is
  used as given."
- `pubsub-arrow/README.md:62-71`: the same two sentences in user terms.
- `d7`: L15 becomes `[built]` for the refusal. Its "Today: clamped … floored" line is replaced by
  "Holds at" (the batcher constructor) and the test names. Its "used as given" clause stays
  `[designed-not-built]` for huge values until D7O-2 and D7O-3. L4's "Until D7O, `max_rows < 1` is
  clamped" note is deleted. §(c)'s last bullet is marked resolved for the refusal. §4's sentence
  saying `Subscribe` refuses out-of-range options is kept.
- `docs/domains/bounded-context-register.md` BC-8: the single site reads "`RecordBatchBatcher`'s
  constructor, the first step of the batched `SubscriberArrow::Subscribe`, before anything is
  subscribed". The C++ clamp copy is marked deleted (D7O-1). The status becomes "check `[built]`
  (D7O-1); sweep owed by D7O-3".

## Corner cases forbidden

Rung 1 (unrepresentable):
- **A batcher with `max_rows < 1` or a negative timeout.** The constructor seal stops one existing,
  whoever constructs it. Today's clamp and the negative-timeout "floor" behaviour therefore cannot
  come back.
- **A second answer to BC-8.** Every batched entry point has to construct a batcher, so none can
  reach the batcher without the one check.
- **A refused call that leaves a thread, a subscription or a held callback behind.** Each of these is
  created only after the constructor returns.
- **Signed overflow at `last_flush_ + milliseconds::min()`** (`cpp:187`). That timeout is now
  refused, so it never reaches the arithmetic.

Rung 2 (refused at the door, typed): `max_rows` ∈ {0, -1, `INT64_MIN`}, timeout ∈ {-1 ms,
`milliseconds::min()`}, and both out of range at once. Every one gets the same error,
`PubSubError(kInvalidArgument)`, and there is no partial mode.

**Precedence, decided here and not put to the owner.** An out-of-range call from a thread that
domain 3 would refuse as re-entrant (`kReentrantCall`, as in
`FailedBatchedSubscribeReleasesItsCallback`) gets `kInvalidArgument`. Both are refusals that leave
nothing behind. Options are checked first because the options check depends only on the arguments,
and it is the simpler order: no reordering is needed. That test uses valid options, so it is
unaffected.

Handled residue: **none.** Zero timeout and `max_rows = 1` are in-range values that the existing
code already serves. They take no branch of their own.

## Premises and stop conditions

1. **`PubSubError` and `PubSubStatus` (`core/.../status.hpp:136`) can be reached from `cpp`
   through its existing includes.** If they cannot, add the include. This is not a stop.
2. **`Subscriber::Subscribe` always calls the provider's options form** (`pubsub/src/subscriber.cpp:445-447`,
   "Always the options form"), so the Mock's `subscribe_with_options_count` (`test:94`) counts provider
   subscriptions. The test makes this observation check itself: it asserts the count is 0 after the
   refusals and 1 after the following valid `Subscribe`. If the valid `Subscribe` does not move the
   count, the observation is void. STOP-AND-ASK: do not drop the assertion. Report it, so that a
   different observation can be chosen.
3. **The batched `Subscribe` (`cpp:371`) is the only place `RecordBatchBatcher` is constructed**
   (it is private, `hpp:175`, and d7 §4 says so). If a second site is found, it inherits the check,
   which is the intent. STOP-AND-ASK only if that site constructs the batcher *after* subscribing or
   starting a thread.
4. **No existing test passes options out of range or relies on clamping** (step-0a audit, CLEAR). If
   the build finds one, it is retired under the two-case rule. A test whose *subject* is the clamp is
   deleted. A test that merely passes such a value is re-anchored to an in-range one. Report either.
5. **Nothing under `c-abi/` or `dotnet/` builds or calls the batched `Subscribe`** (pre-round audit
   part D). STOP-AND-ASK if the change forces any edit there.

## Forcing-test mapping

Both tests go in `test`, suite `SubscriberArrowBatchTest`, using `MockProvider` and `BatchSink`. The
observation window is 200 ms and is one-sided, following the ladder's ground rules.

**Forcing test: `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind`.** The topic is declared. No
other subscription on it exists, so the Mock counter starts at 0. The arms are `max_rows` 0, -1 and
`INT64_MIN`, each with the default timeout; a timeout of -1 ms and of `milliseconds::min()`, each
with `max_rows` 8000; and `{0, -1 ms}`. Each arm:
- hands `Subscribe` a callback that captures a `shared_ptr` token and records into its own sink;
- expects `PubSubError` with `status() == kInvalidArgument`;
- straight after `catch`, expects the token's `weak_ptr` to have expired.

After all the arms, the test checks that the Mock counter is still 0, makes one `Publish`, waits
200 ms, and checks that every refused sink is empty. It then makes a valid `Subscribe` on the same
topic with `max_rows` 1 and expects the counter to read 1. It makes a `Publish`, and
`sink.WaitFor(1, 2 s)` must give one `kRowLimit` batch of 1 row.
- *Turned green by:* the constructor check.
- *Red for the right reason on `8e25cca`:* no arm throws. The clamp keeps `max_rows` and the floor
  keeps the timeout. The `weak_ptr` has not expired because a live batcher holds the callback, the
  counter reads 1, and the refused sinks receive the `Publish`. Each of those assertions is therefore
  shown able to fail.
- *The red run must not depend on undefined behaviour.* An arm whose `Subscribe` does not throw
  records the failure and unsubscribes the id it got back *before* any `Publish`. Without that, the
  `milliseconds::min()` subscription would reach the overflow at `cpp:187` on the old tree.

**Guard (green both ways, declared): `InRangeBatchOptionsAreUsedAsGiven`.** This is the second half
of rung 1's subject.
- *Row-count arm:* `max_rows` 1, timeout 10 min, and 3 `Publish`. `WaitFor(3, 2 s)` must give 3
  `kRowLimit` batches of 1 row each, with `rows_dropped` 0.
- *Zero-timeout arm:* `max_rows` 8000, timeout 0, and one `Publish` after taking `t0 =
  steady_clock::now()`. `WaitFor(1, 2 s)` must give one `kTimeout` batch of 1 row. The callback's
  own `steady_clock::now()` minus `t0` must be at least 1 ms. That is a lower bound, which a
  monotonic clock cannot violate.
- It is green on `8e25cca` too, because neither value was ever clamped. It is declared a guard: it
  proves that the new check accepts both lower bounds and that the batcher still receives them
  unchanged. If the check were written as `<= 0`, or as `max_rows < 2`, this guard would go red.

**Controls.** The forcing test is its own control: every observation it makes is shown red on the
old tree, as listed above. The valid `Subscribe` at its end is the positive control. It shows that a
refusal leaves the topic usable, so "nothing delivered" cannot pass just because the topic is dead.

**No existing test is retired or re-anchored.** This is the step-0a finding.
`FailedBatchedSubscribeReleasesItsCallback` stays as it is.

## Risks / Unknowns

- **Placement differs from the ladder's wording.** The ladder says "at the top of the batched
  `Subscribe` … before the batcher is constructed"; this design puts the check in the batcher's
  constructor. Both meet L15 and D-D7O-3, and the ladder is not tier 1. The reason is given in Design:
  the constructor seal, and the check is inherited by any future entry point. A reviewer who prefers
  the ladder's literal placement is choosing between two equivalent mechanisms, not raising a tier-1
  conflict.
- **Visible change for C++ callers.** A call that used to be clamped or floored now throws. No caller
  in the repository passes such a value (step-0a audit and pre-round audit part C). It is disclosed
  in the brief.
- **C# is not affected in this round.** C# still uses its own batcher, so the refusal reaches C#
  only with the switch round, as the "Refuse both" ruling foresaw. Not D7O's.
- **No coexistence window.** The clamp is deleted in the same edit that adds the refusal. No later
  stage schedules a deletion of anything this stage adds.
- **STOP-AND-ASK: none.** No disagreement was found between tier-1 texts, or between tier 1 and tier 2:
  L15, L4, L5, L9, L11, D15, BC-8 and the 2026-11-12 rulings agree.

## Files-to-touch

- `pubsub-arrow/src/subscriber_arrow.cpp`: the constructor (`:33-38`); the clamp becomes the check.
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`: `BatchOptions` comments and the
  batched `Subscribe` doc (F3, F4, refusal only).
- `pubsub-arrow/README.md`: § Batched RecordBatch subscribe (F5, refusal only).
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: the two tests above.
- `docs/domains/d7-batch-assembly.md`: L15, L4 and §(c), as above.
- `docs/domains/bounded-context-register.md`: the BC-8 row.

**Net lines (production, `cpp` only): +6 / −1.** Tests are about +110. Docs are about +15 / −8 and
are not counted.

## Files-to-delete

- **The clamp `max_rows < 1 ? 1 : max_rows` (`cpp:36`).** Replaced by the refusal in the same
  constructor.
- **The negative-timeout-as-floor behaviour** (`cpp:187`, reachable today). This is a behaviour, not
  a line of code: the behaviour is gone and the change is disclosed. The arithmetic stays for
  in-range values, and stage 2 reworks it.
- **d7 L4's note "Until D7O, `max_rows < 1` is clamped to 1 (`cpp:36`)", and L15's "Today: …" line.**
  Replaced by the `[built]` marks and the "Holds at" text.
- **BC-8's copy entry "`RecordBatchBatcher`'s constructor clamp (`cpp:36`; deleted by D7O)".** It is
  marked deleted, and the site becomes the single-site text.
- **No file or test is deleted.** No test pins the clamp (step-0a audit), and the stage replaces one
  line rather than adding a parallel path.
