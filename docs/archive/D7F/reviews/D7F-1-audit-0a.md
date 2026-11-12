# D7F-1 — test-authority audit, step 0a (MODE=corpus)

**Verdict: CLEAR.** No test in scope asserts, or exercises and accepts, behaviour a tier-1 authority forbids.

**Proof of read:** `plans/RIX-rulings.md` read in full: 169 lines, 14 entries; last entry "2026-11-10 — Queued windows are delivered before the closing batch". Yardstick: those 14 rulings plus d7 §3 L1-L14 (L9 as revised twice). Spec/code and `D7F-pre-round-audit.md` A.5 were not used as authority; this audit reached A.5's conclusion on its own reading.

**Scope:** `test_pubsub_arrow.cpp`: `SubscriberArrowBatchTest.*` ×17 (513-1062), fixtures `BatchSink` (480-505), `MockProvider` (31-105), `MakeRow`; `discard_probe.cpp` (compile-only, no behaviour assertion); `test_roundtrip.cpp` `BatchedRecordBatchDeliveredAcrossDdsBoundary` (214). There are no `DISABLED_`, `GTEST_SKIP` or declared-red rows in scope.

**Checked against the brief's four forbidden subjects:**
- *Pre-schema drops unreported (L1):* no test asserts it. No test publishes before a usable schema exists. That gap is D7F-1's forcing test.
- *Null only means a rejected schema (L7, D9):* not asserted. `NestedDictionarySchemaReportsEveryRowDropped` (861) and `FinishFailureIsReportedNotFatal` (890) assert null for a rejected schema and for a Finish failure, so both fit D9's wording. LAWFUL.
- *kClosing nested inside the calling callback (L9 rev.):* not asserted. `UnsubscribeFromInsideATimeoutFlushIsSafe` (981) is **LEGITIMATE-BUT-ADJACENT**. Its subject (L11 lifetime safety) is lawful, but the comment at 999-1000 ("Stop()'s own kClosing flush ... re-enters this callback once more") states the forbidden nested delivery as expected. The `exchange` guard at 1001 tolerates nesting but does not require it: the window is empty after the timeout flush, so L6 gives no nested delivery, and the test passes without it. RE-ANCHOR the comment and the guard. Rung 6 of `D7F-test-ladder.md` already schedules this, so it does not block D7F-1.
- *Concurrent or out-of-order callbacks (L12):* none is required. `BatchSink` locks each delivery, which tolerates concurrency but does not depend on it. LAWFUL.

**Per-test (the rest):** `FlushesAtRowLimit`, `AttachmentsAlignWithRows`, `CorruptRowIsCountedDroppedAndBatchStaysAligned`, `DictionaryColumn*` ×2, `FixedSizeListWithNamedItemArrivesNonNull`, `BatchesAreValidArrow`, `ReuseAcrossWindows`, `ClosingFlushOnUnsubscribe`, `DroppedRowReportedAndAttachmentDiscarded`, `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly` and `FinishFailureIsReportedNotFatal` are LAWFUL in subject. Several read results synchronously after `Publish` (e.g. the comment at 542), which rides the retired synchronous-delivery construct (D10). That re-anchoring is D7F-3's job, not a finding now. 943's "no delivery after Unsubscribe" holds under revised L9 because its window is empty. `FlushesAtTimeout`, `OnlyDroppedRowsStillDeliversEmptyBatch`, `SubscribeForwardsTopicOptions` and FastDDS 214 are LAWFUL.

**Scheduled restoration:** none. The ladder's rung 6 *removes* the nested-closing comment and guard and does not restore them.

**Laws with no asserting test:** L1 V1/V2 (D7F-1 and D7F-3 forcing tests), L5 cadence, L6 idle case, L9 destructor closing, L10 throwing callback, L12, L13 batched. L14 `[designed-not-built]` has no test and no false green.

**Tier-1 disagreements:** none. L9's text matches both L9 rulings (rul:118-136, 157-169).
