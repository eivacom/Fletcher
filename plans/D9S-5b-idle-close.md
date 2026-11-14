# D9S-5b — Every close ends with exactly one closing delivery (design)

Item: `plans/D9S-plan.md:31`; ladder rung 6 (in-callback and idle close); register BC-5. Authority:
ruling 2026-11-14 (`plans/RIX-rulings.md:478`; ledger read whole, 491 lines); d7 L6, L7, L9
(amended for closing only); d9 L1, L11, L14, L15; audit `plans/reviews/D9S-5b-audit-0a.md`. HEAD `3819962`.

## Summary

Every close of a registered batched subscription delivers exactly one `kClosing`, last, even when
idle. The shape is what d7 L6/L7 already give an empty window: zero-row with a decoder, null without.
The shim changes no code; it forwards whatever domain 7 delivers (d9 L15).

## Design

**One condition in `RecordBatchBatcher::Close`.** `if (!HasPending()) return;` becomes "return only
when idle **and** not an owed closing". The rest is unchanged, so the shape falls out of existing
code: with a decoder, `Finish()` on zero rows gives a non-null zero-row batch of the topic's schema,
no attachments, 0 dropped (P1). With no decoder (no usable schema yet, or rejected for life) it gives
null, 0 dropped; L7 holds, since no batch could be built. If `Finish()` fails: null (L7; residue).

**The obligation starts at registration.** A flag under the batcher's `mu_`, set where
`stop_on_throw.batcher = nullptr` disarms the throw guard (after `batchers_` holds it). A `Subscribe`
that throws returned no subscription, so its guard's `Stop()` keeps today's rule (pending only, never
an empty closing). A C caller told "refused" may free `ctx` at once, and a later empty closing would
then use freed memory. No caller holds the id before the flag is set (P5).

**"Exactly one, last" is today's mechanism.** `Stop()` reaches `Close(kClosing)` once, under `mu_`,
behind `stopped_`. Rows and schemas are refused after `stopped_`. `Loop` exits only when stopped
**and** drained, so the closing comes after any queued windows (L9, D10). D12/D13's exceptions are
unchanged, and now they always end with a delivery. That makes L9's "until the `kClosing` batch has
arrived" a signal a binding can act on.

**The shim: no code.** The thunk already hands over a zero-row batch (non-null array/schema,
`num_rows` 0, `attachments` NULL since there are no rows, d9 L14) and a null one. A destroy inside a
batch callback runs `~fl_subscriber` → `~SubscriberArrow` on the batch thread. That thread pushes no
`DeliveryScope` (BC-6), so nothing terminates (d9 L1). Its own `Stop` returns at once, and the thunk
holds only `(on_batch, ctx)`. No wait, flag or check is added (L15), and there is no new lock order
or throw path.

## Corner cases forbidden

1. **Two closings for one subscription:** unrepresentable (`stopped_` guard under `mu_`).
2. **A closing to a `Subscribe` that threw:** unrepresentable by ordering (obligation after the last
   throw point).
3. **A delivery after the closing:** unrepresentable (queued last; nothing queues after `stopped_`).
4. **A null meaning "no rows" while a schema is usable:** unrepresentable (`Finish()`'s zero-row batch).
5. **The shim inventing, suppressing or waiting for a closing:** no shim code exists (L15, BC-9); the
   edge answers are unchanged (L12, L13).
6. **An idle deadline delivering:** still forbidden; only an owed `kClosing` is exempt (L6 otherwise
   unchanged).

**Handled residue:** `Finish()` fails on an empty window → null, 0 dropped. *Why not forbidden: an
allocation failure cannot be ruled out by type, and L7 names it.* The shim fails to hand over the
closing → lost until D9S-6 (L18, reason kept). *Why not forbidden: an accepted interim, ruling
2026-11-13 (2).*

## Premises and stop conditions

- **P1.** `Finish()` with zero rows returns a non-null zero-row batch (witness
  `AFailedPreSizeDoesNotMakeTheSchemaUnusable`). *STOP-AND-ASK if not: the shape leaves L6/L7.*
- **P2.** `arrow::ExportRecordBatch` exports a zero-row batch. *STOP-AND-ASK if not: the shim would need code.*
- **P3.** The batch thread pushes no `DeliveryScope` (BC-6), and `Subscriber` teardown on it does not
  wait on its own callback. *If F1's in-callback destroy terminates or hangs, stop: shim redesign.*
- **P4.** The edge and probe builds do not link domain 7. *If a BC-10 truth table moves, stop.*
- **P5.** No caller can name a batched subscription before `Subscribe` returns. *If not, stop.*

## Forcing-test mapping

| Test | Kind | Green because | Red before (right reason) |
|---|---|---|---|
| **F1** `Batches.EveryCloseEndsWithExactlyOneClosingDelivery` (c-abi, server binary) | Rung 6, external | The `Close` condition. Cells: {`unsubscribe_batches`, `destroy`} × {outside, inside own callback} × {idle with schema, idle without}. Each cell ends with **exactly one** `CLOSING`, last, 0 dropped. With schema: non-null array and schema, `num_rows` 0, imported schema equals the topic's. Without schema: no array. Inside: the close returns before the closing arrives (count at return = deliveries so far), then the closing arrives within `kAwait`. Trigger: 1 row at `max_rows` 1 (with schema); 1 raw row on an undeclared topic with a 10 ms timeout (without). | Idle cells see 0 `CLOSING`. Sinks are declared before the fixture (5a DEBT-2), so the extra delivery cannot touch a freed sink. |
| F1 controls | Control | Inside × both closers, with a full window queued and 1 row pending: `ROW_LIMIT…, CLOSING(1)`. Green before, so the harness provably observes an in-callback closing and "no wait". One shared 2 s window at the end: no cell gets a later delivery, while one live subscription gets `ROW_LIMIT` (5a pattern). | — |
| **L1** `SubscriberArrowBatchTest.EveryCloseEndsWithExactlyOneClosingDelivery` (d7) | Leg | The same cells via `Unsubscribe` / `~SubscriberArrow`, plus `Unsubscribe` from a row-delivery thread (D13), × {decoder, none}. Becomes the proof that d7 L6/L9 cite. | Idle cells: 0 deliveries. |
| **G1** `FailedBatchedSubscribeReleasesItsCallback` (extended) | Guard | Also asserts that the refused callback was **never** called (counter shared by value). | Green before; red under m2. |
| `DeliveryOrderStress` (tightened) | Guard | "kClosing, if any, last" becomes "exactly one kClosing, last". | Red under m1 on any seed that closes idle. |

Mutation controls, recorded at step 3: **m1** restore the early return (F1/L1 idle cells red);
**m2** create the obligation at construction (G1 red).

## Corpus changes (audit 0a, carried whole)

**Delete** (the subject is forbidden; never re-anchored): `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose`
(`test_pubsub_arrow.cpp:814-826`) and its d7 L6 citation. L1 takes the citation's place.
**Re-anchor: lawful subject, forbidden assertion.** Each expects exactly one empty `kClosing`, last
(zero-row with a decoder, null without): `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly`
(:1404, comment :1369-1371); `UnsubscribeFromInsideATimeoutFlushIsSafe` (:1436, :1432-1434);
`InCallbackUnsubscribeWhilePublishingDoesNotDeadlock` (:2145); `EveryCallbackRunsOnTheBatchThreadInWindowOrder`
(:1757); `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` (:971-973, null);
`EveryAssemblyStateReportsItsDropsAtEitherEnding` via `RunAssemblyCell` (deadline cells);
`PubSubArrowFastDdsTest.BatchedRecordBatchDeliveredAcrossDdsBoundary` (`test_roundtrip.cpp:281`).
**Re-anchor for tolerance: declare the sink before the subscriber.** :524 `SubscribeForwardsTopicOptions`,
:539 `FlushesAtRowLimit`, :562 `FlushesAtTimeout`, :607 `AttachmentsAlignWithRows`, :667
`OnlyDroppedRowsStillDeliversEmptyBatch`, :1115 `DictionaryColumnRefoldedToDictionaryArray`, :1156
`DictionaryColumnPreservesNulls`, :1191 `CorruptRowIsCountedDroppedAndBatchStaysAligned`, :1262
`FixedSizeListWithNamedItemArrivesNonNull`, :1459 `BatchesAreValidArrow`, :2446
`InRangeBatchOptionsAreUsedAsGiven` (both arms; :2470-2476). These land in a commit **before** the
code change. They are green both ways, so no commit holds a use-after-free.
**Watch** (stays green only because the shape is zero-row with a decoder, P1): `…ThenLateSchemaAssembles`
:891, `MaxRowsLargerThanAnyWindowIsUsedAsGiven` (a) :2852.

## Texts to correct (in the code commit)

- **d7:** L6 gains "except at closing: every close ends with exactly one `kClosing` (L9), a zero-row
  batch, or null if there is no decoder"; its Tests line cites L1. L9's "delivered once" becomes
  "exactly one `kClosing` ends the subscription, empty when nothing is pending" (note "Revised
  2026-11-14"). D1 narrowed; new **D18** cites 2026-11-14. Findings (c): deadline case tested
  (:924-929), closing case reversed (L1). `cpp:234-238` comment.
- **pubsub-arrow:** README :74, :114-115; `subscriber_arrow.hpp` :115, :176-177.
- **binding.h** :867-877, :1091-1093: exactly one `CLOSING` (empty when idle) is the last use of
  `on_batch`/`ctx`; from inside a batch callback the close returns at once and `ctx` may be freed in
  or after that delivery. "Idle one delivers nothing", "no reliable end signal" and "must not destroy
  from inside" are deleted (superseded by the ruling).
- **c-abi tests** comments `test_batches.cpp:515-517`, :689. **d9:** L1 annotation and L15 → `[built]`
  (F1); §1 :48, §2 :85; register BC-5 shim note. **Ladder** rung 6: "even when nothing is pending".

## Risks / Unknowns

- **Visible to C++ callers of `SubscriberArrow`:** an idle close now delivers one empty `kClosing`.
  Zero-row and null batches were already lawful (L6, L7); no API changes. The C# batcher (domain 10)
  is untouched.
- **Pre-existing, reported, not fixed (beyond L6-for-closing):** a `Subscribe` that throws after
  rows reached its batcher still delivers them as `kClosing` (practically unreachable). Also: an
  allocation failure queueing the closing inside a destructor's `Stop` terminates; idle closes now
  reach that too.
- **For D9S-6:** L18's no-batch hand-over must keep `CLOSING` as the reason, or the end signal is lost.
- **No stop-and-ask trigger fires:** d7 changes only L6/L9's closing clause; no edge answer, status or
  channel; no shim window, timer or check; no domain 2 or `dotnet/` edit; the behaviour change is only
  on the unreleased (0.8) server batched path. No coexistence window.

## Files-to-touch

`pubsub-arrow/{src/subscriber_arrow.cpp, include/fletcher/pubsub_arrow/subscriber_arrow.hpp, README.md,
tests/test_pubsub_arrow.cpp}`, `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp`,
`c-abi/{include/fletcher/abi/binding.h, tests/test_batches.cpp}`,
`docs/domains/{d7-batch-assembly,d9-c-abi-shim,bounded-context-register}.md`, `plans/D9S-test-ladder.md`.

**Declared net lines.** Production code: domain 7 **+12 / −3**; shim **+0 / −0** (doc comments not
counted: d7 hpp/README ~+14/−10, binding.h ~+12/−14). Tests **~+360 / −60** (F1 ~170, L1 ~110, G1 8,
re-anchors ~40/−15, tolerance ~22/−22, stress 3/−2, deletion −11). New public surface: **0**.

## Files-to-delete

Test `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose`: behaviour gone, disclosed (citation → L1).
binding.h's "no reliable end signal" / "must not destroy from inside": replaced by the guaranteed
closing. No file, type or config key retires; one early return narrows.
