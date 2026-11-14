# D9S-5b — Every close ends with exactly one closing delivery (design)

Item: `plans/D9S-plan.md:31`; ladder rung 6 (in-callback and idle close); register BC-5. Authority:
ruling 2026-11-14 (`plans/RIX-rulings.md:478`; ledger read whole, 491 lines); d7 L6, L7, L9
(amended for closing only); d9 L1, L11, L14, L15; audit `plans/reviews/D9S-5b-audit-0a.md`. HEAD `3819962`.

## Summary

Every close of a registered batched subscription delivers exactly one `kClosing`, last, even when
idle. Its shape is d7 L6/L7's: zero-row with a decoder, null without. The shim changes no code (d9
L15). What a **refused** subscribe may deliver is owner Decision 1 (brief).

## Design

**One condition in `RecordBatchBatcher::Close`.** `if (!HasPending()) return;` becomes "return only
when idle **and** not an owed closing". The rest is unchanged, so the shape falls out of existing
code. A decoder exists once a row with a usable schema has arrived (L13), so a subscription closed
before its first row, even on a declared topic, gets null. With a decoder, `Finish()` on zero rows
gives a non-null zero-row batch of the topic's schema, no attachments, 0 dropped (P1). With none, it
gives null, 0 dropped: L7 holds, because no batch could be built. If `Finish()` fails: null (residue).

**The obligation starts at registration:** a flag under `mu_`, set where `stop_on_throw.batcher =
nullptr` disarms the throw guard. Before that no *empty* closing is owed, and the guard's `Stop()`
delivers only what is pending (P5).

**A refused subscribe can still deliver (review B1; verified).** Domain 3 publishes the new entry
(`subscriber.cpp:597-599`) before two late refusals: an options conflict on a topic already subscribed
(`:324-325`), and a provider that delivered once and then failed (`:604-636`). Rows arriving in
between reach `AddRow`. Windows may be delivered before the refusal, and the guard's `Stop` queues a
`kClosing` holding the rest. From an ordinary thread that arrives **before** the refusal returns.
From inside a row delivery it arrives **after**, so a binding that frees `ctx` on refusal then uses
freed memory. Nothing signals either case. The reentrant door (`:329`) and L15's refusal deliver
nothing (no provider subscription, no thread). Moving domain 3's check is outside the round, and it
would not cover provider failure. **Per option (Decision 1):** **(a)** `Loop` holds deliveries until
registration, and an unregistered `Stop` discards its window and queue; d7 L1 needs an owner-widened
clause. **(b)** As above; `binding.h` says that after `FL_INVALID_ARGUMENT` or a transport failure,
deliveries may follow with no end signal, so `ctx` must live for the process. **(c)** Drop the flag.
Every refusal made before a batcher starts (shim arguments, the edge build, L15, thread start) also
calls `on_batch` once.

**"Exactly one, last" is today's mechanism.** `Stop()` reaches `Close(kClosing)` once under `mu_`,
behind `stopped_`. `Loop` exits only when stopped **and** drained (L9, D10). D12/D13's exceptions
now always end with a delivery, so L9's "until the `kClosing` batch has arrived" is a usable signal.

**The shim: no code** under (a) and (b). The thunk already hands over a zero-row batch (non-null
array and schema, `num_rows` 0, `attachments` NULL, d9 L14) and a null one. A destroy inside a batch
callback runs `~SubscriberArrow` on the batch thread, which has no `DeliveryScope` (BC-6), so nothing
terminates (d9 L1). The thunk holds only `(on_batch, ctx)`. No wait, check or lock order is added (L15).

## Corner cases forbidden

1. **Two closings for one subscription:** unrepresentable (`stopped_` guard under `mu_`).
2. **An *empty* closing to a `Subscribe` that threw:** unrepresentable by ordering. A closing that
   *holds rows* is not forbidden: that is B1, Decision 1.
3. **A delivery after the closing:** unrepresentable (queued last; nothing queues after `stopped_`).
4. **A null meaning "no rows" while a schema is usable:** unrepresentable (`Finish()`'s zero-row batch).
5. **The shim inventing, suppressing or waiting for a closing:** no shim code (L15, BC-9; L12, L13).
6. **An idle deadline delivering:** still forbidden; only an owed `kClosing` is exempt (L6).

**Handled residue:** `Finish()` fails on an empty window → null, 0 dropped. *Why not forbidden: an
allocation failure cannot be ruled out by type; L7 names it.* The shim fails to hand over the closing
→ lost until D9S-6 (L18). *Why not forbidden: an accepted interim, ruling 2026-11-13 (2).*

## Premises and stop conditions

- **P1.** `Finish()` with zero rows returns a non-null zero-row batch (witness
  `AFailedPreSizeDoesNotMakeTheSchemaUnusable`). *STOP-AND-ASK if not: the shape leaves L6/L7.*
- **P2.** `ExportRecordBatch` exports a zero-row batch. **P4.** Edge and probe do not link domain 7.
  *If either is false (the shim needs code, or a BC-10 truth table moves), stop.*
- **P3.** The batch thread pushes no `DeliveryScope` (BC-6; review: verified). *If F1's in-callback
  destroy terminates or hangs, stop.* **P5.** No caller names a batched id before `Subscribe` returns.

## Forcing-test mapping

| Test | Kind | Green because | Red before (right reason) |
|---|---|---|---|
| **F1** `Batches.EveryCloseEndsWithExactlyOneClosingDelivery` (c-abi, server binary) | Rung 6, external | The `Close` condition. Cells: {`unsubscribe_batches`, `destroy`} × {outside, inside own callback} × {idle after a decoded row, idle before any row}, plus **(d)** `unsubscribe_batches` from a row delivery on another topic of the same provider (DEBT-2). Each cell ends with **exactly one** `CLOSING`, last, 0 dropped. After a decoded row: non-null array and schema, `num_rows` 0, imported schema equals the topic's. Before any row (declared or not): no array. Inside and (d): the close returns before the closing arrives (count at return = deliveries so far), then it arrives within `kAwait`. Trigger: 1 row at `max_rows` 1 (decoded); 1 raw row on an undeclared topic with a 10 ms timeout (none). | Idle cells see 0 `CLOSING`. Sinks are declared before the fixture (5a DEBT-2). |
| F1 controls | Control | Inside × both closers, with a full window queued and 1 row pending: `ROW_LIMIT…, CLOSING(1)`. Green before, so the harness provably observes an in-callback closing and "no wait". One shared 2 s window at the end: no cell gets a later delivery, while one live subscription gets `ROW_LIMIT` (5a pattern). | — |
| **L1** `SubscriberArrowBatchTest.EveryCloseEndsWithExactlyOneClosingDelivery` (d7) | Leg | The same cells via `Unsubscribe` / `~SubscriberArrow`, plus `Unsubscribe` from a row-delivery thread (D13), × {decoder, none}. Becomes the proof that d7 L6/L9 cite. | Idle cells: 0 deliveries. |
| **G1** `FailedBatchedSubscribeReleasesItsCallback` (extended) | Guard | Also asserts that the refused (reentrant-door) callback was **never** called (counter shared by value). Holds under (a) and (b); under (c) it expects one `kClosing`. | Green before; red under m2. |
| **F2** `Batches.ARefusedBatchedSubscribeNeverDelivers` (c-abi, server; **(a) only**) | Forcing (a) | Conflicting topic options on a busy topic, refused from outside and from inside a row delivery: the callback is never called (2 s window, with a live control). Under (b) no test is named: a test would pin the tolerance. Under (c) it becomes "exactly one `CLOSING` per refusal". | Today a row-holding `CLOSING` arrives. |
| `DeliveryOrderStress` (tightened) | Guard | "kClosing, if any, last" becomes "exactly one kClosing, last". | Red under m1 on any seed that closes idle. |

Mutations (step 3): **m1** restore the early return (idle cells red); **m2** owe from construction (G1 red).

## Corpus changes (audit 0a, carried whole)

**Delete** (subject forbidden; never re-anchored): `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose` (:814-826).
**Re-anchor: lawful subject, forbidden assertion.** Each expects exactly one empty `kClosing`, last
(zero-row with a decoder, null without): `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly`
(:1404, comment :1369-1371); `UnsubscribeFromInsideATimeoutFlushIsSafe` (:1436, :1432-1434);
`InCallbackUnsubscribeWhilePublishingDoesNotDeadlock` (:2145); `EveryCallbackRunsOnTheBatchThreadInWindowOrder`
(:1757); `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` (:971-973, null);
`EveryAssemblyStateReportsItsDropsAtEitherEnding` via `RunAssemblyCell` (deadline cells);
`PubSubArrowFastDdsTest.BatchedRecordBatchDeliveredAcrossDdsBoundary` (`test_roundtrip.cpp:281`).
**Re-anchor for tolerance: declare the sink before the subscriber** (audit lines). `SubscribeForwardsTopicOptions`,
`FlushesAtRowLimit`, `FlushesAtTimeout`, `AttachmentsAlignWithRows`, `OnlyDroppedRowsStillDeliversEmptyBatch`,
`DictionaryColumnRefoldedToDictionaryArray`, `DictionaryColumnPreservesNulls`, `CorruptRowIsCountedDroppedAndBatchStaysAligned`,
`FixedSizeListWithNamedItemArrivesNonNull`, `BatchesAreValidArrow`, `InRangeBatchOptionsAreUsedAsGiven`
(both arms). They land **before** the code change, green both ways.
**Watch** (stays green only because the shape is zero-row with a decoder, P1): `…ThenLateSchemaAssembles`
:891, `MaxRowsLargerThanAnyWindowIsUsedAsGiven` (a) :2852.

## Texts to correct (in the code commit)

- **d7:** L6 gains "except at closing: every close ends with exactly one `kClosing` (L9), a zero-row
  batch, or null if there is no decoder"; its Tests line cites L1. L9's "delivered once" becomes
  "exactly one `kClosing` ends the subscription, empty when nothing is pending" (note "Revised
  2026-11-14"). D1 narrowed; new **D18** records the ruling only (the registration ordering is code,
  not law). Findings (c): deadline case tested (:924-929), closing case reversed. `cpp:234-238`.
- **pubsub-arrow:** README :74, :114-115; `subscriber_arrow.hpp` :115, :176-177.
- **binding.h** :867-877, :1091-1093: exactly one `CLOSING` (empty when idle) is the last use of
  `on_batch`/`ctx`; from inside a batch callback or a row delivery, the close returns at once and `ctx`
  may be freed in or after that delivery. "Idle one delivers nothing", "no reliable end signal" and
  "must not destroy from inside" are deleted. The subscribe doc says what a refusal may have
  delivered, per Decision 1.
- **c-abi tests** comments `test_batches.cpp:515-517`, :689. **d9:** L1 annotation and L15 `[built]`, row-delivery
  clause included (F1 (d)); §1 :48, §2 :85; BC-5. **Ladder** rung 6: "even when nothing is pending".

## Risks / Unknowns

- **Visible to C++ callers:** an idle close now delivers one empty `kClosing`. A callback whose
  captures die before the subscriber now uses freed memory even when idle (11 tests here did; the
  README already forbids it). No API change; the C# batcher (domain 10) is untouched.
- **STOP-AND-ASK, Decision 1 (B1):** (a) widens d7 by an L1 clause; (c) changes an edge answer (L12)
  and d7 L15. Only (b) stays inside the round. Pre-existing: a failed allocation queueing the closing
  inside a destructor's `Stop` terminates the process, and idle closes now reach it too. For D9S-6:
  L18 must keep `CLOSING` as the reason. Otherwise no trigger fires; no coexistence window.

## Files-to-touch

`pubsub-arrow/{src/subscriber_arrow.cpp, include/fletcher/pubsub_arrow/subscriber_arrow.hpp, README.md,
tests/test_pubsub_arrow.cpp}`, `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp`,
`c-abi/{include/fletcher/abi/binding.h, tests/test_batches.cpp}`,
`docs/domains/{d7-batch-assembly,d9-c-abi-shim,bounded-context-register}.md`, `plans/D9S-test-ladder.md`.

**Declared net lines, production code:** (a) d7 **+18 / −3**, shim 0. (b), the default: d7
**+12 / −3**, shim 0. (c) d7 **+10 / −2**, shim **~+18**. Doc comments are not counted. **Tests
~+385 / −60:** F1 ~195 including (d); L1 ~110; G1 8; re-anchors ~40/−15; tolerance ~22/−22; stress
3/−2; deletion −11; plus F2 ~+45 under (a). New public surface: **0**.

## Files-to-delete

Test `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose`: behaviour gone, disclosed (its citation
moves to L1). binding.h's "no reliable end signal" and "must not destroy from inside" are replaced.
