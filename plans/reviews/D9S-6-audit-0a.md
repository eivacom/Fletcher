# D9S-6 step 0a: test-authority audit (MODE=corpus), HEAD 2221b8f

**Verdict: CLEAR.** No test in scope is FORBIDDEN or FORBIDDEN-BY-TOLERANCE.

**Proof of read:** `plans/RIX-rulings.md` read in full: 502 lines (`wc -l`); last entry at l.493, "2026-11-14 — D9S-5b: a refused batched subscribe never delivers".

**Yardstick:** d9 L1, L2, L7, L14, L15, L18, Q12; d7 L1, L2, L9, L10. All rulings, especially: 2026-11-13 "a failed hand-over is reported" (L18), 2026-11-13 D9S-4 (2) ("lost uncounted during D9S-4/5 only"; that window has now closed), 2026-11-14 "every close ends with one closing delivery", and 2026-11-14 "a refused batched subscribe never delivers".

**Scope covered:**
- `c-abi/tests/test_batches.cpp`: all 10 TESTs (l.444-1534), plus the helpers `BatchSink`, `MoveOrLeaveSink`, `CloseCell`, `CountingSink`, `ExpectWindows` and `ExpectRefusedAsEdge`.
- `test_shim_batches_nothing.cpp`: l.70 and l.94.
- `test_containment.cpp`: all 6 `Contain` tests.
- pubsub-arrow, read only: `FinishFailureIsReportedNotFatal`, `CorruptRowIsCountedDroppedAndBatchStaysAligned`, `ReuseAcrossWindows`, `RowsConservedAtEveryEnding`, `ARefusedSubscribeNeverDeliversWhatItCaught`.
- No test is `#[ignore]`d or `DISABLED_`, and no row is declared red.

**What was checked:**
- **Lost uncounted:** no test in scope does it. No test reaches the thunk's failure path at all; that needs D-D9S-4's fault hook, which does not exist yet. Every test that counts rows requires all of them to be accounted for (`AWindowThatCouldNotBeBuilt...` l.576, d7 `RowsConservedAtEveryEnding`).
- **Silent handoff:** no test asserts or accepts one. Every sink callback is deliberately `noexcept` and never throws (l.103-105, l.139), so nothing exercises d7 L10's swallow path through the shim.
- **Lawful exception:** `ARefusedBatchedSubscribeNeverDelivers` (l.1320) and its d7 twin accept rows discarded uncounted. That is lawful: they are rows caught by a refused subscribe, ruled 2026-11-14 and written into the d7 L1 clause.
- **Where the expired gap lives:** only in code, in `c-abi/src/batches_server.cpp:103-106` and the empty `catch (...)` at :138-139. That is tier 3 and this stage's subject, not a test finding.
- **Scheduled restoration:** none found.
- **Tier-1 disagreement:** none.

**Laws with no test (gaps, not violations):**
- d9 L18 `[designed-not-built]` has no test, as expected; rung 8 is its forcing test.
- No test covers a failed hand-over of a **CLOSING** window, which must still arrive with reason CLOSING (2026-11-14 ruling). Rung 8 should cover it.
- d7 L10's throwing callback is untested (d7 says so).
- No test pins the hand-over origin for Q12; that half of Q12 is still open.
- d9 L1 has no test that drives every entry point with a throwing body.
