# D7O-1 — step-0b test-authority audit (MODE=new-test)

**Verdict: CLEAR.** Base `555d899`, working-tree diff of `pubsub-arrow/tests/test_pubsub_arrow.cpp`.

**Proof-of-read.** `plans/RIX-rulings.md`: 285 lines, 23 entries, last heading "2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows". d7 §3 L1-L15 read in full.

**Scope (all categories).**
- `SubscriberArrowBatchTest.OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` (test:2346-2443). This is the forcing test. Its six arms are `max_rows` 0, -1, `INT64_MIN` and timeout -1 ms, `milliseconds::min()`, plus both out of range. It checks: `kInvalidArgument`; the `weak_ptr` expired straight after the catch; the Mock counter at 0; refused sinks empty over a 200 ms window. **LAWFUL.** It asserts L15 word for word ("refused with an invalid-argument error before anything is subscribed: no thread, no provider subscription, no delivery") and the ruling "Refuse both". Its red-path `Unsubscribe` of the `milliseconds::min()` arm runs only after `ADD_FAILURE`. It is hygiene for undefined behaviour on the old tree, as DEBT-1 asks. It does not tolerate the behaviour, because the arm has already failed.
- The positive control in that test is a valid `RowLimitOptions(1)` `Subscribe`: counter 0→1, one `kRowLimit` batch of 1 row. **LAWFUL** (L4, L15 "used as given"). It also confirms premise 2: the counter moves on a real subscription.
- `SubscriberArrowBatchTest.InRangeBatchOptionsAreUsedAsGiven` (test:2445-2497). This is the guard. Its first arm uses `max_rows` 1 with a 10 min timeout and expects 3×1-row `kRowLimit` batches with `rows_dropped` 0. Its second arm uses timeout 0 and expects one `kTimeout` batch, arriving at least 1 ms after `Publish`. **LAWFUL.** L15: "Every value it accepts is used as given". L5: "after a 1 ms floor". d7 Q5: "A zero timeout is accepted (the 1 ms floor applies)". The lower bound is one-sided, so it cannot be flaky-red.
- Neither test pins the clamp or treats a negative timeout as the floor. These are the two behaviours L15 forbids. No tests are ignored or declared red, and no new helpers are added (`BatchSink`, `RowLimitOptions` and `MockProvider` are pre-existing and lawful).

**Law (b).** Every arm is driven through the public `SubscriberArrow::Subscribe` and `Unsubscribe` and `PublisherArrow::Publish`. `RecordBatchBatcher` is never called. `subscribe_with_options_count` is read only.

**New VALUE subject beyond rung 1: none.** No no-timeout spelling (rung 2). No `milliseconds::max()` or other huge timeout (rung 3). No `INT64_MAX` `max_rows` (rung 4). `INT64_MIN` and `milliseconds::min()` are rung-1 arms. No plan change, no stop-and-ask.

**Scheduled restoration:** none. **Tier-1 disagreement:** none (L4, L5, L9, L11, L15, D15 and the 2026-11-12 rulings agree).

**Laws with no test** (outside this rung, for the record): L5's cadence timing, L10's throwing callback, L14 (`[designed-not-built]`, owed by RIX).
