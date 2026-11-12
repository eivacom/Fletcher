# D7O-1 — step 0a test-authority audit (MODE=corpus)

**Verdict: CLEAR.** Tree `8e25cca`, branch `feature/d7o-batch-options`, 2026-11-12.

**Yardstick (tier 1):** `plans/RIX-rulings.md` read in full: **285 lines, 23 entries, last heading "2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows"**. Also `docs/domains/d7-batch-assembly.md` §3 L1-L15 and D14-D15. Main checks: L15 ("refused ... before anything is subscribed ... Every value it accepts is used as given"), L4, L5 (1 ms floor; no timeout; "never an early or wrapped" deadline), and ruling "Refuse both" (2026-11-12).

**Scope covered:** every `BatchOptions` construction and batched `Subscribe` in
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: all 38 `SubscriberArrowBatchTest.*` tests, plus helpers `NoTriggerBatchOptions` (:701), `DeadlineBatchOptions` (:837), `RowLimitOptions` (:1703), the guards `DeliveryOrderStress` (:2149) and `RowsConservedAtEveryEnding` (:2232), and `FailedBatchedSubscribeReleasesItsCallback` (:2301);
- `pubsub-arrow/tests/test_pubsub_arrow_heavy.cpp:81` (`heavy`), and `pubsub-arrow/tests/discard_probe.cpp:85` (`BatchOptions{}`, compile-only);
- `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp:243`, and `arrow-bridge/benchmarks/bench_arrow_codec.cpp:514` (Args 1000 and 8000).
- `integration-tests/fastdds-xrce-interop/tests/test_interop.cpp` has no batched `Subscribe`, so nothing in it is in scope.

**Findings:**
- No test is disabled, skipped or declared red. No test mentions a clamp.
- Every `max_rows` passed is between 1 and 100000. Every timeout is between 1 ms and 10 min, or the defaults (8000, 1 min). No test passes `max_rows < 1`, a negative timeout, a zero timeout, or a huge value, and no test relies on clamping. All are **LAWFUL**. Nothing needs re-anchoring.
- `FailedBatchedSubscribeReleasesItsCallback` is **LAWFUL**. It covers a `kReentrantCall` refusal with valid options, and L15 does not touch that.
- **Scheduled restoration:** none. The ladder ("rung 1 re-anchors nothing") and pre-round audit part C agree.
- **Laws with no test:**
  - **L5** is marked `[built]` for finite timeouts, but nothing tests its cadence or the 1 ms floor. `FlushesAtTimeout` only checks that a timeout flush happens.
  - **L10**'s throwing-callback half has no test.
  - Both gaps were already known (d7 §(c)). D7O-1 rung 1's zero-timeout arm covers part of the 1 ms floor.
  - L15 is `[designed-not-built]` and has no test, which is consistent: D7O owes that test.
- No tier-1 disagreement.
