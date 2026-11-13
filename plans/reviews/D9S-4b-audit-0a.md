# D9S-4b step-0a test-authority audit (corpus): CLEAR

**Verdict: CLEAR.** No test in scope is FORBIDDEN or FORBIDDEN-BY-TOLERANCE. Nothing to delete, no re-anchors, no scheduled restoration, no tier-1 disagreement. HEAD 0297b69, light lane.

**Proof of read:** `plans/RIX-rulings.md` has 476 lines. Its last entry, at line 470, is "2026-11-13 — D9S: tighter document caps for the remaining stages".

**Checked against:** every ruling in the ledger; d9 §3 L1-L18 (especially L3, L11, L12-L15 and L18); d7 L3 ("`attachments.size() == batch rows`, and `attachments[i]` belongs to batch row `i`. A dropped row takes its attachment with it.") and d7 L14; ladder rung 4. The spec and the code were used only for context.

**Scope covered (all LAWFUL):**
- `c-abi/tests/test_batches.cpp`: `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` (:282), `.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted` (:363), `.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` (:435); fixtures `BatchSink`, `PublishedRows`, `BatchFixture`; the reason `static_assert`s. No disabled or skipped case. None asserts that a batch arrives *without* attachments, so none tolerates the 4a gap or resists 4b.
- `c-abi/tests/test_shim_batches_nothing.cpp`: `ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer` (:70) and its control `.TheMatcherFindsEveryPatternItIsGivenInItsOwnSource` (:94). They enforce L15 and BC-9.
- The row path's attachment tests: the 8 `Attachments.*` cases in `test_binding_entry_points.cpp` (:919-1107), `Subscriber.AttachmentsReachTheHandlerAndARetainedBlobOutlivesTheDelivery` (:1417), and `ProbeFixture.ALoanedAttachmentIsDeliveredByAddress` / `.AMissingLoanedAttachmentScoresAsACopy` (`test_probe_shim.cpp`:209, :228). These cover L3, L9 and L11: borrowed for the call, kept by `fl_blob_retain`.
- `pubsub-arrow/tests/test_pubsub_arrow.cpp` (read only): `SubscriberArrowBatchTest.AttachmentsAlignWithRows` (:607), `.DroppedRowReportedAndAttachmentDiscarded` (:633), `.CorruptRowIsCountedDroppedAndBatchStaysAligned` (:1191), `PubSubArrowTest.PublishWithAttachments` (:406). These are d7 L3's named tests, and 4b's design ("one `fl_attachments` per row, an empty set rather than a NULL entry") agrees with them.

**Manifest, debt and declared-red:** none owned by 4b. `design-debt.md`:850 (DEBT-4) only records the 4a/4b dependency.

**Build-state notes (for 4b's docs, not findings against tests):**
- d9 L14's attachment clause and Q6's attachment half are `[designed-not-built]` with no test yet. The 4b forcing test is meant to become that test.
- d9 L14's "release what was not moved" is marked built in part with no test. That belongs to rung 5 (D9S-5), not this stage.
- d7 L3 `[built]` has tests. d7 L14 and d9 L17/L18 are `[designed-not-built]`, and no test claims them as built.
