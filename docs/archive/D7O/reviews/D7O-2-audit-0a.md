# D7O-2 — step 0a test-authority audit (corpus)

**Verdict: CLEAR.** No test in scope asserts, or exercises and accepts, behaviour a tier-1 authority forbids. Nothing to delete or re-anchor. No scheduled restoration. No tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md`, 285 lines, 23 entries, last heading "2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows". Also read `docs/domains/d7-batch-assembly.md` §3 (L1–L15, D14), `plans/D7O-test-ladder.md` rungs 2–3, and `plans/D7O-locked-decisions.md`, all at `9cd04de`.

**Yardstick:** L4 says a window closes at `max_rows`, the deadline ("only for a subscription that has a timeout"), the budget or closing. L5 says "a subscription may have no timeout … a timeout too long for the clock to represent means no deadline, never an early or wrapped one". L6 says "a window that contains only drops is still delivered". L1, L9 and L15 ("every value it accepts is used as given") also apply, with rulings 2026-11-12 "Add 'never' to C++", "Refuse both" and "D7O hand-over" (LC-1).

**Scope covered:**
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: every `SubscriberArrowBatchTest.*` (:538–:2497), the helpers `NoTriggerBatchOptions` (:705), `DeadlineBatchOptions` (:841), `RowLimitOptions` (:1707), `RunAssemblyCell` (:996) and `RunClosingFromInside`, and the guards `EveryAssemblyStateReportsItsDropsAtEitherEnding`, `DeliveryOrderStress` and `RowsConservedAtEveryEnding`. It also includes D7O-1's `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` (:2346) and `InRangeBatchOptionsAreUsedAsGiven` (:2445).
- `test_pubsub_arrow_heavy.cpp:66`, `pubsub-arrow/tests/discard_probe.cpp:85`, `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp:243`, and `arrow-bridge/benchmarks/bench_arrow_codec.cpp:514`.
- No test in scope is disabled, skipped or declared red.

**Checked and found lawful:**
- **No test pushes a huge timeout.** No test asserts that a huge timeout fires early: the largest timeout used is 10 min, and no test uses `milliseconds::max()` or another unrepresentable value.
- **No test assumes a deadline.** No test asserts that every subscription has a deadline. The only reads of the default (`BatchOptions{}.timeout`, :2352) are filler in arms that are refused.
- **Lost-only windows are reported on a clock only when a timeout was set.** The tests that do this are `OnlyDroppedRowsStillDeliversEmptyBatch`, both `*AtEachDeadline` tests and the deadline cells of the guard. Each sets a finite timeout of 100 ms. L5 gives such a subscription a deadline, and L6 requires the delivery.
- **The emulation helpers are LAWFUL, not adjacent.** `NoTriggerBatchOptions`, `RowLimitOptions` and the `minutes(10)` / `seconds(10)` / `minutes(1)` literals stand in for "no timeout" with a finite timeout the clock can represent. That timeout is used as given and its deadline comes at its true time, which is L15 and L5 for finite timeouts. The tests' subjects (row limit, closing, alignment, conservation) are lawful. Moving them onto the new no-timeout option is optional. If that is done, keep finite-timeout coverage.

**Laws vs. build-state marks:**
- **No false green.** No test asserts the no-timeout case or the unrepresentable case as built. Both are `[designed-not-built]`, and rungs 2–3 owe their tests.
- **Gaps.** L5's finite cadence is `[built]` but has no test (d7 says so itself). L10's throwing-callback half is `[built]` but has no test. L14 is `[designed-not-built]`, and RIX owes its test.
