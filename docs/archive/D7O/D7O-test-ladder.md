# D7O — test ladder

Round **D7O**, domain 7 (batch assembly), branch `feature/d7f-csharp-sync`. Purpose (owner): a
subscriber can ask for batches that close only when they are full or when it unsubscribes, never on a
clock; and a subscriber that asks for nonsensical batching (fewer than one row per batch, a negative
timeout) is told so at once instead of silently getting something else. This lets C#, whose batched
subscription already offers "no timeout" and already refuses a negative timeout, move onto the C++
batcher without losing either (ruling 2026-11-12, one batch assembler, C++'s; D16).

Citations: `d7` = `docs/domains/d7-batch-assembly.md`, `rul` = `plans/RIX-rulings.md`,
`reg` = `docs/domains/bounded-context-register.md`, `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`,
`hpp` = `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`,
`test` = `pubsub-arrow/tests/test_pubsub_arrow.cpp`, `bd` = `arrow-bridge/src/batch_decoder.cpp`.
Line numbers are those of the tree at `ff5be3e`.

Ceiling: **250 net production lines per stage** (tests not counted). Sizes are estimates of net
production lines. Doc text (header, README, d7) is listed but not counted.

**Out of scope (ruling 2026-11-12, rul:236-245):** C#, the C ABI shim, BC-2's per-row gap, BC-7. No
rung touches them.

## Ground rules for every rung

- **Driven from outside:** the batched `SubscriberArrow::Subscribe(segments, RecordBatchCallback,
  BatchOptions, TopicOptions)`, `Unsubscribe` and `~SubscriberArrow`, fed by `PublisherArrow::Publish`
  / `PublishDirect`. `RecordBatchBatcher` is never called by a test.
- **Providers:** the file-local `MockProvider` (test:36), which delivers synchronously and hands a
  null schema for a topic never declared, and `InProcessPubSubProvider`. Both exist; no new test
  infrastructure is needed.
- **How "no timeout" is spelled is the architect's decision.** This ladder names no type or constant.
  ⚠ **One tier-1 constraint on that choice:** L15 refuses every negative timeout, so "no timeout"
  cannot be spelled as a negative duration (as C#'s `Timeout.InfiniteTimeSpan`, -1 ms, is). A
  spelling that is a negative timeout value would be refused by rung 1's own rule.
- **Timing assertions are one-sided, so they cannot be flaky-red.** "Nothing delivered" is asserted
  over a fixed observation window (the architect picks it; ~200 ms is ample), followed by "exactly X
  at `Unsubscribe`". A slow machine can only make such an assertion pass more easily, never fail it.
  Each such rung carries a **control arm**: the same body with a short finite timeout (e.g. 10 ms)
  must get a `kTimeout` within a generous bound (2 s, `sink.WaitFor`), which shows the observation
  window is long enough to see a deadline that does exist. Lower bounds ("not before 1 ms") are safe
  to assert, because a deadline cannot fire early on a monotonic clock. No upper bound tighter than
  the 2 s `WaitFor` convention is asserted.
- **"Nothing was subscribed" is observed from outside, with one internal observation.** Outside: the
  callback handed to a refused `Subscribe` is released by the time the throw reaches the caller (a
  `weak_ptr` to something it captured has expired, as in `FailedBatchedSubscribeReleasesItsCallback`,
  test:2301); a `Publish` on the topic afterwards delivers nothing; a later valid `Subscribe` on the
  same topic works normally. Inside (observation only): the Mock's subscription counter
  (`subscribe_with_options_count`, test:103, or the callback map) shows no provider subscription.

## The ladder

| # | Subject (what becomes TRUE) | External path | Internal observation | Builds on | New work (est. net prod. lines) | Authority |
|---|---|---|---|---|---|---|
| **1** | **A batched `Subscribe` with `max_rows` below 1 or a negative timeout is refused at once with `PubSubError(kInvalidArgument)`, and leaves nothing behind**: no batch thread holds its callback, no provider subscription exists, nothing is ever delivered. Every in-range value is accepted and used as given: `max_rows` = 1 closes a window per row, and a **zero timeout is accepted**, with a pending window delivered as `kTimeout` no sooner than 1 ms after its row (the floor). | Mock: batched `Subscribe` with `max_rows` 0, -1, `INT64_MIN`, then with a timeout of -1 ms and `milliseconds::min()`, then with both out of range. Each throws `kInvalidArgument`. After each, the captured `weak_ptr` has expired (checked straight after the catch, no polling), a `Publish` delivers nothing within the observation window, and a following valid `Subscribe` on the same topic receives that `Publish`'s row normally. Accepted arms: `max_rows` 1 with 3 rows gives 3 `kRowLimit` batches of 1 row; timeout 0 with 1 row gives one `kTimeout` batch of 1 row, and the elapsed time from before `Publish` to the callback is at least 1 ms. | The Mock's subscription counter stays unchanged across a refused call: it observes "no provider subscription"; the call itself is driven through `SubscriberArrow`. | The D7F pipeline as built. `FailedBatchedSubscribeReleasesItsCallback` already proves a failed `Subscribe` can release its callback; this rung refuses **before** anything is built, so there is nothing to release. | The range check at the top of the batched `Subscribe` (cpp:366), before the batcher is constructed (cpp:371) or `Start`ed. Delete the clamp `max_rows < 1 ? 1 : max_rows` (cpp:36). **~10-20.** Docs: `hpp:77-79` and `hpp:99-` state the refusal and the zero/1 ms rule; README likewise; d7 L15 and L4 marked `[built]`, §(c) last bullet resolved. Red today: no throw (clamped / floored). | **L15**; D15 (rul:226-234); L4's "until D7O" note (d7:111-112); L5 (1 ms floor); reg BC-8 |
| **2** | **A subscription that asks for no timeout gets windows that close only when full or at closing, never on a clock.** Full windows still arrive as `kRowLimit` at once; the remainder waits, however long, and arrives as the `kClosing` batch. **A window holding only lost rows is reported only at closing**, counted in the `kClosing` batch's `rows_dropped` (L1); lost rows do not count toward `max_rows`. | (a) InProcess or Mock, declared schema, no timeout, `max_rows` 3. `Publish` 7 rows: `sink.WaitFor` 2 `kRowLimit` batches (rows 0-2, 3-5); then nothing within the observation window; then `Unsubscribe` delivers exactly one `kClosing` batch with row 6, and nothing after. Repeat with `~SubscriberArrow` as the ending. (b) No usable schema (Mock, topic never declared): 5 `PublishDirect` rows, `max_rows` 2; nothing within the window; `Unsubscribe` gives one null `kClosing` batch, `rows_dropped` 5. (c) Declared schema, 5 malformed rows, `max_rows` 2: nothing within the window; `kClosing` zero-row batch, `rows_dropped` 5. Control arm: (a)-(c) with a 10 ms timeout get a `kTimeout` within 2 s. | None. | Rung 1: the range check is where the no-timeout spelling is accepted (it must not be refused). D7F's queue, loop and closing path (cpp:116-136, 233-256), unchanged. | The no-timeout spelling in `BatchOptions` (architect's choice), carried into the batcher; `ArmTimer` (cpp:184-191) arms no deadline for such a subscription, so `Loop` waits on `cv_` only (cpp:249-250) and `AddRow`'s deadline check (cpp:92) never fires. **~10-25.** Docs: `hpp` BatchOptions and batched-`Subscribe` comment, README, d7 L4/L5 no-timeout case `[built]`, §4. Red today: the option cannot be expressed (compile-level red); the control arm shows the assertion can fail behaviourally. | **L4, L5** (no-timeout clauses); D14 (rul:218-224); **L1, L6** (lost-only window reported, at closing); L9 |
| **3** | **A finite timeout too long for the clock to represent never brings an early or wrapped deadline: it behaves as no timeout.** A very long timeout that the clock can represent gives a deadline at its true time (so, within the test, never). | Same body as rung 2 (a), with the timeout set to, in turn: `milliseconds::max()`; `duration_cast<milliseconds>(steady_clock::duration::max())` (converts exactly, but `last_flush + timeout` overflows); and a long representable value (e.g. 100 years). Each: `kRowLimit` batches at once, nothing more within the observation window, then exactly one `kClosing` with the remainder. Red today: for `milliseconds::max()`, the conversion to the clock's nanoseconds wraps to -1 ms (`(2^63-1) x 10^6 mod 2^64`), so `std::max(now + 1 ms, last_flush - 1 ms)` (cpp:187) gives a `kTimeout` about 1 ms after the first row. The architect confirms each value's red on the pre-stage tree and states which are already green (a guard arm, declared). | None. | Rung 2: "no deadline" exists as a state of the batcher, so an unrepresentable deadline maps onto it rather than inventing a second idle path. | `ArmTimer`'s deadline arithmetic made overflow-safe: when `last_flush_ + timeout_` cannot be represented, arm no deadline. **~5-15.** Docs: d7 L5 unrepresentable case `[built]`, and the "Until D7O ... overflows" note (d7:125-126) deleted. | **L5** ("a deadline is never earlier than its timeout says"); D14; L15 ("used as given") |
| **4** | **A `max_rows` larger than any window will ever reach is used as given: rows are assembled and delivered, never dropped because of the size asked for.** | Declared schema, `max_rows` = `INT64_MAX`, a 10 ms timeout (and, as a second arm, no timeout): `Publish` 3 rows; `sink.WaitFor` one `kTimeout` batch with 3 rows and `rows_dropped` 0 (second arm: nothing within the window, then `kClosing` with 3 rows). Red today (expected; the architect confirms): `SetSchema` pre-sizes the decoder to `max_rows` (cpp:72), `BatchDecoder::Reserve(INT64_MAX)` fails (bd:600-602), and the `catch (...)` at cpp:73-75 discards the decoder, so the subscription treats a decodable schema as unusable for life: every row arrives as a null batch with `rows_dropped` 3. | None. | Rung 1 (the clamp is gone, so `max_rows` reaches the batcher as given); rung 2 (the no-timeout arm). | A failed or oversized pre-size is not fatal in `SetSchema`, as it already is not in `Close` (cpp:214-221); the pre-size is bounded or skipped. The mechanism is the architect's. **~5-15.** Docs: none beyond d7 L15's "Holds at". | **L15** ("every value it accepts is used as given"); **L8** (only an undecodable schema is permanent; this one is decodable); reg BC-8 ("the largest representable") |

**Rungs: 4. Stages: 3.** Stage 1 is rung 1. **Stage 2 is rungs 2-3.** Stage 3 is rung 4 plus the
BC-8 sweep (below). Every stage is far under the ceiling (largest ~25-40 net lines).

**Why stage 2 carries two rungs (flagged; not a ceiling exception).** Rung 3's subject ("an
unrepresentable deadline is no deadline") reuses the state rung 2 creates, and depending on how the
architect spells "no timeout", rung 3 may arrive partly green with rung 2 (for instance, if the
largest duration is the spelling, its own arm is green by construction; the
`steady_clock::duration::max()` arm is still red). Alone it would be a stage of ~5-15 lines that might
have no red work. The two stay separate rungs so each subject is proven separately; the architect
orders stage 2's red-first work so each rung's test is shown red on the pre-stage tree, and declares
any arm that is green both ways as a guard.

**Accumulation and deletion checks.** No rung depends on a construct a later rung creates. No rung
builds what a later rung deletes: *(as landed, D7O-2: the range check needed no extension; `kNoTimeout` is non-negative and passes it as is)* stage 1's range check is extended (not replaced) by stage 2 to
accept the no-timeout spelling; stage 1 deletes the clamp, which stage 3 needs gone.

## Tests other than rungs

- **BC-8 sweep (reg:22), stage 3; a declared guard.** Over `max_rows` {`INT64_MIN`, -1, 0, 1, 2,
  `INT64_MAX`} x timeout {`milliseconds::min()`, -1 ms, 0, 1 ms, no timeout, 100 years,
  `steady_clock::duration::max()` in ms, `milliseconds::max()`}: refused ⇔ `max_rows < 1` or timeout
  negative; a refused call leaves no callback held, no provider subscription and no delivery; an
  accepted call delivers what it was given (one row published, then `Unsubscribe`: Σ`num_rows` +
  Σ`rows_dropped` = 1, and no `kTimeout` before the 1 ms floor). The sweep asserts only the
  one-sided timings of the ground rules. It lands last because three of its values only mean
  something after rungs 2-4.
- **Controls.** Each rung is deterministically red on the pre-stage tree:
  - 1: no throw (the value is clamped or floored).
  - 2: does not compile (the option does not exist); the 10 ms control arm shows the "nothing
    delivered" assertion fails when a deadline exists.
  - 3: `kTimeout` about 1 ms after the first row (`milliseconds::max()` arm).
  - 4: a null batch with `rows_dropped` 3 instead of 3 rows.
- **Existing tests.** No test in the repo passes `max_rows < 1` or a negative timeout (searched), so
  rung 1 re-anchors nothing. `FailedBatchedSubscribeReleasesItsCallback` stays as it is.

## Findings and owner questions

- **Q1 (owner) — resolved 2026-11-12, "Fix in D7O": rung 4 stays.** Rung 4 was found while laddering, not handed down. A very large `max_rows` today
  silently turns a decodable schema into "unusable for life" (cpp:72-75): every row is dropped and
  reported. That is the "silently getting something else" the round exists to remove, and L15 ("used
  as given") and BC-8's sweep ("the largest representable") already require it, so this ladder treats
  it as in scope rather than a widening. If the owner prefers to defer it, rung 4 and the
  `INT64_MAX` row of the sweep move to a later round and stage 3 shrinks to the sweep alone, with that
  row marked as a known gap.
- **No tier-1 disagreement found.** L4, L5, L15, D14, D15, BC-8 and the 2026-11-12 rulings agree with
  each other. The one constraint they put on the design (no negative spelling of "no timeout") is
  stated in the ground rules.
- **For the C# switch round, not D7O:** C# today refuses a timeout above `BatchOptions.MaxTimeout`
  (~49.7 days; `dotnet/src/Fletcher/SubscriberArrow.cs:228-235`). Under L5 and L15 C++ accepts any
  non-negative timeout and treats an unrepresentable one as no deadline, so C# callers passing such
  values will see them accepted after the switch. That is a visible C# change the switch round should
  record; it is not a D7O rung.
