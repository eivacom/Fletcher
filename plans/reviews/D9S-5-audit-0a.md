# D9S-5 — step 0a test-authority audit (corpus)

**Verdict: BLOCKED** (one tier-1 disagreement, below). No test in scope is FORBIDDEN or FORBIDDEN-BY-TOLERANCE today; MUST-DELETE(0). No scheduled restoration found.
**Proof of read:** `plans/RIX-rulings.md`, 476 lines, top to bottom; last entry line 470, "2026-11-13 — D9S: tighter document caps for the remaining stages". Yardstick: every ruling; d9 §3 L1–L18; d7 §3 L1–L15. HEAD 6d270f0.

## The tier-1 disagreement (owner arbitrates; not resolved here)
Case: the subscription is closed from inside its own batch callback (Unsubscribe or destroy) or from a row-delivery thread, and **nothing is pending**.
- Ruling 2026-11-11 (1): "queued batches, then the closing batch, arrive after the callback returns, and the callback must outlive them". Ruling (2): "the remaining batches, then the closing batch, still arrive in order on the batch thread, and the caller may not free what those callbacks use until the closing batch arrives". The chosen D7F-3 option texts (`docs/archive/D7F/D7F-3-brief.md:38-46`) also have no idle qualifier.
- d7 L6: "A window with nothing pending delivers nothing." d7 L9 says "The pending window is delivered once, as `kClosing`", yet keeps the ruling's "may not free … until the `kClosing` batch has arrived".
- For this case the texts disagree literally. The ruling says a closing batch arrives; L6 says nothing is delivered. Under L6, the ruling's free point never comes. The owner was never asked about the idle case. The 2026-11-10 ruling is scoped "while rows are pending"; the 2026-11-11 rulings are not. Domain 7's code and tests follow L6, and `binding.h:867-877` now says CLOSING "is NOT guaranteed … no reliable end signal". That is the L6 reading, chosen without a ruling.
- **It bites rung 6** (the in-callback and in-delivery arms), which this stage builds. Either a closing batch always ends the subscription (L6 and L9 amended), or the ruling's free point covers only a non-empty window, and C callers get some other end signal or none (the ruling or L9 amended). The owner decides whether rungs 5, 7 and rung 6's outside-close arms may start first.
- **Tests at stake, all LAWFUL under L6:** d7 `UnsubscribeFromInsideATimeoutFlushIsSafe` (`pubsub-arrow/tests/test_pubsub_arrow.cpp:1414`, "the Unsubscribe below owes no closing batch") and `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly` (`:1374`, exactly 1 delivery). Both would become FORBIDDEN if the owner rules "closing always". They are domain 7's and read-only here.

## Known tension: what asserts "CLOSING always"
- **No test asserts it.** Every inherited CLOSING expectation has rows pending: `test_batches.cpp:311`, `:490` (row 5 pending); d7 rung-6 cases (a)–(e) (`:2077`, each has rows pending); `ClosingFromAnotherThread…` (`:1920`, "kClosing, if any, is last").
- **Texts to correct if L6 stands (tier 3, or annotation):** tracker `plans/D9S-plan.md:30` ("closing delivers the rest then the closing batch"); ladder rung 6 (`plans/D9S-test-ladder.md:61`, "the remaining batches, then the closing batch, still arrive"); the d9 L1 annotation bullet ("the queued batches, then the closing batch, are still delivered"). Also `binding.h:875` ("A binding must not destroy from inside a batch callback") steers bindings away from a path that ruling 2026-11-11 (1) makes lawful. The owner question should cover it.

## Classification (scope as declared)
| Test | File:line | Class |
|---|---|---|
| `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` | c-abi/tests/test_batches.cpp:311 | LAWFUL (d9 L14, L15; d7 L5, L9, L12). Partial rung 6 and rung 7 arms; per D9S-4a 0b cycle 2, **not** grounds to mark them green |
| `Batches.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted` | :392 | LAWFUL (d9 L14; d7 L4, L6, L7) |
| `Batches.EachBatchRowCarriesItsOwnAttachments` | :490 | LAWFUL (d9 L14; d7 L3) |
| `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` (the window test) | :565 | LAWFUL (d9 L12, L16, BC-10). Its 10 ms TIMEOUT arm is not the BC-8 or BC-14 grid |
| `BatchSink` (moves every batch), `BatchFixture` (l.259 "then the partial one (reason closing)") | :60-280 | LAWFUL helpers. The sink never leaves a batch unmoved, so rung 5's release arm is untested |
| `ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer`, `.TheMatcherFindsEveryPattern…` | test_shim_batches_nothing.cpp:70, :94 | LAWFUL (d9 L11, L15, BC-9) |
| `Subscriber.NullArgumentsAreRefusedAndCancellingNothingIsNotAnError` (unknown-id no-op) | test_binding_entry_points.cpp:1705 | LAWFUL (d9 L4) |
| d7 L9: `ClosingFlushOnUnsubscribe` :585, `NoUsableSchemaDropsReportedAtDestruction` :765, `SchemaDeclaredAfterSubscribe…` :798, `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose` :816, the two at stake above, `ClosingFromAnotherThread…` :1920, `UnsubscribeFromInsideACallbackNeverDeliversNested` :2077 | pubsub-arrow/tests/test_pubsub_arrow.cpp | LAWFUL (d7 L6, L9; rulings 2026-11-10 and 2026-11-11 for the pending cases) |
| d7 L15 and BC-8: `OutOfRangeBatchOptions…` :2347, `InRangeBatchOptionsAreUsedAsGiven` :2446, `NoTimeoutWindows…` :2680, `TimeoutTooLongForTheClock…` :2707, `EveryBatchOptionIsRefusedExactlyWhenOutOfRange` :3055 | same | LAWFUL (d7 L4, L5, L15; "Refuse both") |

No re-anchor is needed. No test is LEGITIMATE-BUT-ADJACENT or UNMOORED.

## Declared scope not found
- **No c-abi test exists for row-path no-wait-in-delivery** (an `fl_subscriber_unsubscribe` from inside a row delivery returns at once: ruling 2026-11-11 (2), and domain 3's 2026-09-04 rule as forwarded by d9 L4). **None exists for termination on destroy from a row delivery either** (d9 L1, "the designed exception"; it has no test). Nothing in scope pins them, lawful or not.

## Laws against build marks
- `[built]` but untested: the d9 L1 designed termination; d9 L2 (enums agree, as marked).
- Owed, and correctly marked `[designed-not-built]`: the d9 L14 unmoved-release clause (rung 5, BC-13); the d9 L15 option pass-through and refusal (rung 7, BC-8 grid, BC-14); d9 L15 "closing follows d7 L9" (rung 6, BC-5 shim note).
- No false green found.
