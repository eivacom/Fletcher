# D9S-5b — step 0a test-authority audit (corpus)

**Verdict: MUST-DELETE(1).** 7 tests must be re-anchored because of a forbidden assertion and 11 because of tolerance. No scheduled restoration. Not BLOCKED: the owner's 2026-11-14 ruling already settles the tier-1 collision ("d7 L6 is amended for closing"). L6's text is stale until 5b amends it, and that amendment is ordered work, not an open disagreement.
**Proof of read:** `plans/RIX-rulings.md`, 491 lines, read top to bottom. Highest entry at line 478: "2026-11-14 — D9S-5: every close ends with one closing delivery; D9S-5 splits". Yardstick: every ruling; d7 §3 L1–L15 (L6 as amended for closing); d9 L1, L11, L14, L15. HEAD ad69f4a.
**Yardstick line:** "every close of a batched subscription ends with exactly one closing delivery, even when nothing is pending." d7 L6 still governs non-closing windows: an idle window at a deadline delivers nothing, and that stays lawful.

## MUST-DELETE (subject forbidden)
| Test | File:line | Why |
|---|---|---|
| `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose` | pubsub-arrow/tests/test_pubsub_arrow.cpp:816 (assert :825) | Its whole subject is "idle close delivers nothing" (comment :814-815), which the ruling forbids. Its secondary L7 concern ("null never means no rows") does not survive as a reason to keep it. It passes to 5b's forcing test, which must fix the empty delivery's shape (the ruling leaves this open). Cited as proof in d7 L6 Tests (:147); remove it there. Nothing schedules its return. |

## RE-ANCHOR: lawful subject, forbidden assertion beside it (expect exactly one empty kClosing, last)
| Test | File:line | Assertion to change |
|---|---|---|
| `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly` | test_pubsub_arrow.cpp:1374 | `delivery_count == 1` (:1404), comment :1369-1371 "no closing batch to deliver". Subject: L9/L11 in-callback close. |
| `UnsubscribeFromInsideATimeoutFlushIsSafe` | :1414 | `EXPECT_FALSE(second)` (:1436), comment :1432-1434 "owes no closing batch". Subject: L11 self-detach. |
| `InCallbackUnsubscribeWhilePublishingDoesNotDeadlock` | :2103 | `recs.size() == 1` (:2145). Subject: BC-5 deadlock guard. |
| `EveryCallbackRunsOnTheBatchThreadInWindowOrder` | :1720 | Expected list of 3×kRowLimit (:1757). `sub.reset()` (:1750) closes an idle window. Subject: D10/L12. |
| `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` | :954 | `deliveries.size() == after_w2` after Unsubscribe (:971-973). Subject: L1/L6 at deadlines. |
| `EveryAssemblyStateReportsItsDropsAtEitherEnding` (via `RunAssemblyCell`) | :1066 / :1043 | In the deadline cells, "exactly these: not none, not more" counts no closing after an idle Unsubscribe. Subject: L1/L8. |
| `PubSubArrowFastDdsTest.BatchedRecordBatchDeliveredAcrossDdsBoundary` | integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp:215 | `deliveries.size() == 1` "exactly one row-limit flush" (:281) after an idle Unsubscribe. Subject: batch thread fed over DDS. |

## FORBIDDEN-BY-TOLERANCE → RE-ANCHOR (declare the sink before the subscriber)
These tests declare `BatchSink sink` (or capture locals) after `SubscriberArrow sub` and close only through the destructor, with the last window idle. Today they are safe only because an idle close delivers nothing. Under the ruling, the closing delivery writes into a sink that has already been destroyed (use-after-free). So the tests resist removal of the forbidden behaviour, and all their subjects are lawful. `SubscribeForwardsTopicOptions` :524, `FlushesAtRowLimit` :539, `FlushesAtTimeout` :562, `AttachmentsAlignWithRows` :607, `OnlyDroppedRowsStillDeliversEmptyBatch` :667, `DictionaryColumnRefoldedToDictionaryArray` :1115, `DictionaryColumnPreservesNulls` :1156, `CorruptRowIsCountedDroppedAndBatchStaysAligned` :1191, `FixedSizeListWithNamedItemArrivesNonNull` :1262, `BatchesAreValidArrow` :1459, `InRangeBatchOptionsAreUsedAsGiven` :2446 (both arms; the zero-timeout arm's `mu`/vectors at :2470-2476).

## LAWFUL (scope covered)
- d7: `ClosingFlushOnUnsubscribe`, `ReuseAcrossWindows`, `DroppedRowReported…`, `NestedDictionary…`, `FinishFailure…`, the four `*AtUnsubscribe`/`*AtDestruction`, `ClosingFromAnotherThread…` :1920, `UnsubscribeFromInsideACallbackNeverDeliversNested` :2077 (a)–(e) (each case has rows pending, so kClosing is already expected), `RowsConservedAtEveryEnding`, `NoTimeout…`, `TimeoutTooLong…`, `ShortTimeoutControl…`, `AFailedPreSize…`, `OutOfRange…`, `EveryBatchOptionIsRefused…`, `FailedBatchedSubscribe…`, heavy `OverBudgetRow…` (row pending). The idle deadline stretch in `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` :924-929 is lawful: L6 is unamended for non-closing windows.
- **Shape-dependent (watch at design):** `…ThenLateSchemaAssembles` (`ASSERT_NE(batch, nullptr)` :891 applies to any trailing empty closing) and `MaxRowsLargerThanAnyWindowIsUsedAsGiven` (a) (`nulls == 0` :2852). Both stay green only if an idle close on a decoder-holding subscription is a non-null zero-row batch, which is what d7 L7 already requires ("never 'no rows'").
- `DeliveryOrderStress` :2154 is tolerant ("kClosing, if any", :2153) and does not resist the ruling. It should tighten to "exactly one, last" (optional).
- c-abi `test_batches.cpp`, all 9 tests and the helpers: LAWFUL. Every sink is declared before `BatchFixture`, and every count is taken before teardown (DEBT-2). `ClosingFromOutside…` (:690) and the truth tables already avoid any idle-close assertion. The `AWindowThatCouldNotBeBuilt…` comment at :516-517 is stale ("a delivery with nothing pending is never made, d7 L6") but its asserts do not depend on it: the snapshot is taken before close.
- Out of scope: the C# managed batcher in `preview-bundle-consumer` belongs to domain 10.

## Texts to correct (tier 2/3; none edited here)
- **d7 L6** :142-149: the law text ("A window with nothing pending delivers nothing") needs a closing exception per the ruling. Its Tests line cites the test to be deleted. d7 Findings (c) :447 and D1 ("a partial window is flushed as kClosing") need narrowing. A new D-row should cite 2026-11-14.
- **d7 L9** :186: "The pending window is delivered once, as `kClosing`" should read "exactly one `kClosing`, empty when nothing is pending".
- **binding.h** :867-877 (`fl_subscriber_destroy`): "(an idle one delivers nothing)", "CLOSING is NOT guaranteed … no reliable end signal", "A binding must not destroy from inside a batch callback". All are superseded (the ruling says so). Also :1091-1093 (`fl_subscriber_unsubscribe_batches`: "if the last window holds rows or drops (an idle one: …)").
- **pubsub-arrow README** :74 ("whatever is left arrives") and :114-115 ("then the partial one"); header `subscriber_arrow.hpp` :115 and :176-177 ("the partial one"); code comment `subscriber_arrow.cpp:237` ("an idle window yields nothing (L6)"). These read as conditional on rows being pending.
- **Ladder rung 6** (`D9S-test-ladder.md:61`): it does not contradict the ruling but is silent on the idle close. Add "even when nothing is pending" to the subject and the idle arms. **d9 L1 annotation** (:139-143): consistent with the ruling; only its `[designed-not-built]` mark is owed. **d9 L15** :386, d9 §1 :48/:85, BC-5: correctly mark the idle close as 5b's. **c-abi/README** :238-242: already consistent (it says "then the closing batch" with no condition).

## Laws against build marks
- After the deletion, no test asserts the amended L6/L9 closing clause. That is expected: it is 5b's forcing test. It must cover an idle close from outside, from inside a callback, and by destroy, plus the empty delivery's shape.
- Existing gaps, unchanged: d7 L10 (throwing callback) is untested. d7 L14 is `[designed-not-built]`, correctly. d7 Findings (c) lists "No delivery for an idle window (L6)" as untested, but :924-929 tests the deadline case, so that entry is stale.
