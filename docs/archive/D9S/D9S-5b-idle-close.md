# D9S-5b — Every close ends with exactly one closing delivery (design)

Item: `plans/D9S-plan.md:31`; ladder rung 6 (in-callback and idle close); register BC-5. Authority:
rulings 2026-11-14 (`plans/RIX-rulings.md:478`) and 2026-11-14 "a refused batched subscribe never
delivers" (`:493`; ledger read whole, 502 lines); d7 L1 (refused clause), L6, L7, L9 (closing only);
d9 L1, L11, L14, L15; audit `plans/reviews/D9S-5b-audit-0a.md`; review B1. HEAD `3819962`.

## Summary

Every close of an **accepted** batched subscription delivers exactly one `kClosing`, last, even when
idle: zero-row with a decoder, null without (d7 L6/L7). A **refused** subscribe never delivers, so
`ctx` may be freed on return (`:493`). The shim changes no code (d9 L15).

## Design

**One flag, `registered_`,** under `mu_`. It is set, and `cv_` notified, where `stop_on_throw.batcher
= nullptr` disarms the throw guard, after `batchers_` holds the batcher and past the last throw point
(P5). It drives three rules:
1. **`Close`:** `if (!HasPending()) return;` becomes "return only when idle and not (`kClosing` and
   `registered_`)". With a decoder (one exists once a row with a usable schema arrived, L13),
   `Finish()` on zero rows gives a non-null zero-row batch of the topic's schema, 0 dropped (P1).
   Without one: null, 0 dropped (L7). A close before the first row, even on a declared topic, is null.
2. **`Loop` delivers only once `registered_`.** Earlier windows wait in order (D10, L12). If the
   batcher is stopped while unregistered, `Loop` clears the queue and ends.
3. An unregistered `Close(kClosing)` returns at once and queues nothing (4b S1; a second gate behind rule 2, DEBT-4 (b)).

**Why the gate (B1, verified).** Domain 3 publishes the entry (`subscriber.cpp:597-599`) before two
late refusals: an options conflict on a subscribed topic (`:324-325`), and a provider that delivered
and then failed (`:604-636`). Rows in between reach `AddRow`. Today they can be delivered before the
refusal, or as a `kClosing` after it if the refusal came from a row delivery. Moving domain 3's check
is outside the round, and it would not cover provider failure.

**Exactly one, last** is today's mechanism: `Stop()` reaches `Close(kClosing)` once behind
`stopped_`, and `Loop` exits only when stopped and drained (L9, D10). D12/D13 now always end with a delivery.

**The shim: no code.** The thunk already hands over zero-row batches (non-null array and schema,
`attachments` NULL, d9 L14) and null ones. A destroy inside a callback runs `~SubscriberArrow` on the
batch thread, which has no `DeliveryScope` (BC-6; d9 L1). The thunk holds only `(on_batch, ctx)`. There
is no new wait, check, lock or throw path (L15).

## Corner cases forbidden

1. **Two closings for one subscription:** unrepresentable (`stopped_` guard under `mu_`).
2. **Any delivery to a refused `Subscribe`:** unrepresentable (delivery gated on registration, rule 2; an unregistered close queues nothing, rule 3).
3. **A delivery after the closing:** unrepresentable (queued last; nothing queues after `stopped_`).
4. **A null meaning "no rows" while a schema is usable:** unrepresentable (`Finish()`'s zero-row batch).
5. **The shim inventing, suppressing or waiting for a closing:** no shim code exists (L15, BC-9; L12, L13).
6. **An idle deadline delivering:** still forbidden; only an accepted subscription's `kClosing` is exempt (L6).

**Handled residue:** `Finish()` fails on an empty window → null, 0 dropped. *Why not forbidden: an
allocation failure cannot be ruled out by type; L7 names it.* The shim fails to hand over the closing
→ lost until D9S-6 (L18). *Why not forbidden: an accepted interim, ruling 2026-11-13 (2).*

## Premises and stop conditions

- **P1.** `Finish()` with zero rows is a non-null zero-row batch (witness `AFailedPreSizeDoesNotMakeTheSchemaUnusable`).
  **P2.** `ExportRecordBatch` exports it. *If either is false, STOP-AND-ASK: the shape or the shim changes.*
- **P3.** The batch thread pushes no `DeliveryScope` (BC-6, verified). *If F1's in-callback destroy
  terminates or hangs, stop.* **P4.** Edge and probe do not link domain 7. **P5.** No caller names a
  batched id before `Subscribe` returns. *If either is false, stop.*
- **P6.** A test provider can deliver inside its own `Subscribe` and then throw, reaching the published
  entry (`subscriber.cpp:314-316`, `:626-628`). *If not, D7-2 has no trigger: stop before relying on F2 alone.*

## Forcing-test mapping

| Test | Kind | Green because | Red before (right reason) |
|---|---|---|---|
| **F1** `Batches.EveryCloseEndsWithExactlyOneClosingDelivery` (c-abi, server binary) | Rung 6, external | Rule 1. Cells: {`unsubscribe_batches`, `destroy`} × {outside, inside own callback} × {idle after a decoded row, idle before any row}, plus **(d)** `unsubscribe_batches` from a row delivery on another topic of the same provider (DEBT-2). Each cell ends with **exactly one** `CLOSING`, last, 0 dropped. After a decoded row: non-null array and schema, `num_rows` 0, imported schema equals the topic's. Before any row (declared or not): no array. Inside and (d): the close returns before the closing arrives (count at return = deliveries so far), then it arrives within `kAwait`. Trigger: 1 row at `max_rows` 1 (decoded); 1 raw row on an undeclared topic with a 10 ms timeout (none). | Idle cells see 0 `CLOSING`. Sinks are declared before the fixture (5a DEBT-2). |
| F1 controls | Control | Inside × both closers, with a full window queued and 1 row pending: `ROW_LIMIT…, CLOSING(1)`. Green before, so the harness provably observes an in-callback closing and "no wait". One shared 2 s window at the end: no cell gets a later delivery, while one live subscription gets `ROW_LIMIT` (5a pattern). | — |
| **F2** `Batches.ARefusedBatchedSubscribeNeverDelivers` (c-abi, server binary) | Ruling `:493`, external | Rules 2 and 3. A publisher floods busy topic T, where a batched subscription with options X is live. `max_rows` 1, N = 200 attempts, each a batched subscribe to T with conflicting options Y, from outside and from inside a row delivery on another topic. Each answers `FL_INVALID_ARGUMENT`, and its own sink (a separate `ctx` per attempt) is **never** called, across a final 2 s window. | Caught rows arrive as `ROW_LIMIT`/`CLOSING`. The race is probabilistic; step 3 records the observed red rate. |
| F2 control | Control | During the same flood, a subscribe to T with **matching** options X gets a `ROW_LIMIT` within 2 s. That proves the flood reaches a newly published entry, so F2's silence means something. | — |
| **D7-1** `SubscriberArrowBatchTest.EveryCloseEndsWithExactlyOneClosingDelivery` (d7) | Leg | F1's cells via `Unsubscribe` / `~SubscriberArrow`, plus `Unsubscribe` from a row-delivery thread (D13), × {decoder, none}. Becomes d7 L6/L9's cited proof. | Idle cells: 0 deliveries. |
| **D7-2** `SubscriberArrowBatchTest.ARefusedSubscribeNeverDeliversWhatItCaught` (d7) | Leg, deterministic | A test provider delivers rows inside `Subscribe`, then throws (P6). Cells: `max_rows` 1 (a full window was queued) and `max_rows` 100 (rows pending). The refusal is rethrown, the callback is never called, and the callback is released. Becomes d7 L1's refused-clause proof. | Today: a `kRowLimit` or `kClosing` holding the caught rows. |
| **G1** `FailedBatchedSubscribeReleasesItsCallback` (extended) | Guard | The reentrant-door refusal's callback is **never** called (counter shared by value). | Green before; red under m2. |
| `DeliveryOrderStress` (tightened) | Guard | "kClosing, if any, last" becomes "exactly one kClosing, last". | Red under m1 on any seed that closes idle. |

**Mutations** (step 3 records each red): **m1** restore `Close`'s early return → F1/D7-1 idle cells.
**m2** set `registered_` at construction → G1, D7-2, F2. **m3** delete `Loop`'s gate → D7-2 `max_rows`
1, F2 (`max_rows` 100 stays green: rule 3 masks it). **m4** delete rule 3's guard: equivalent, masked by rule 2 (DEBT-4 (b)).

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

- **d7:** L6 gains "except at closing: every close of an accepted subscription ends with exactly one
  `kClosing` (L9), zero-row, or null if there is no decoder" (Tests: D7-1). L9's "delivered once"
  becomes "exactly one `kClosing` ends an accepted subscription, empty when nothing is pending"
  ("Revised 2026-11-14"). **L1 gains:** "A refused `Subscribe` delivers nothing: rows caught by a
  subscription refused before it was registered are discarded uncounted (ruling 2026-11-14, `:493`)"
  (Tests: D7-2). D1 narrowed. **D18** cites `:478`, **D19** `:493`, recording the rulings only.
  Findings (c): deadline case tested (:924-929), closing case reversed. `cpp:234-238` comment.
- **pubsub-arrow:** README :74, :114-115; `subscriber_arrow.hpp` :115, :176-177; add (DEBT-5): no batch
  is delivered unless the call succeeds; the first may arrive while it is returning.
- **binding.h** :867-877, :1091-1093: exactly one `CLOSING` (empty when idle) is the last use of
  `on_batch`/`ctx`; from inside a batch callback or a row delivery the close returns at once, and `ctx`
  may be freed in or after that delivery. Delete "idle one delivers nothing", "no reliable end
  signal" and "must not destroy from inside". :1060-1064 (DEBT-5): no batch unless the call succeeds; the first may
  arrive while it returns, before `out_id` is written: correlate by `ctx`. A refused call delivers nothing ever.
- **c-abi tests** comments `test_batches.cpp:515-517`, :689. **d9:** L1 annotation and L15 `[built]`, incl.
  the row-delivery clause (F1 (d)); §1 :48, §2 :85; BC-5. **Ladder** rung 6: "even when nothing is pending".

## Risks / Unknowns

- **Visible to C++ callers:** an idle close delivers one empty `kClosing`, and no batch unless `Subscribe`
  succeeds (the first may arrive while it returns, DEBT-5). A callback whose captures die before the subscriber now uses freed memory even
  when idle (11 tests did; the README forbids it). No API change; the C# batcher is untouched.
- **Accepted loss (`:493`):** rows a refused subscription caught are discarded uncounted. The topic's
  accepted subscriptions still get them.
- **F2 is a race.** If step 3 never sees it red before the change, F2 stays as a guard, and D7-2 with
  m3 carries the red. That is reported at close.
- **Pre-existing:** an allocation failure queueing a closing in a destructor's `Stop` terminates the
  process. **D9S-6:** L18 keeps `CLOSING` as the reason. **Scope:** d7 changes only L6/L9 for closing
  and L1's refused clause, as ruled. No trigger fires otherwise. No coexistence window.

## Files-to-touch

`pubsub-arrow/{src/subscriber_arrow.cpp, include/fletcher/pubsub_arrow/subscriber_arrow.hpp, README.md,
tests/test_pubsub_arrow.cpp}`, `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp`,
`c-abi/{include/fletcher/abi/binding.h, tests/test_batches.cpp}`,
`docs/domains/{d7-batch-assembly,d9-c-abi-shim,bounded-context-register}.md`, `plans/D9S-test-ladder.md`.

**Declared net lines.** Production code: domain 7 **+20 / −4**; shim **+0 / −0**. Doc comments are
not counted (d7 hpp/README ~+16/−12, binding.h ~+18/−16). **Tests ~+470 / −60:** F1 ~195 including
(d); F2 ~70; D7-1 ~110; D7-2 ~50 (with its test provider); G1 8; re-anchors ~40/−15; tolerance
~22/−22; stress 3/−2; deletion −11. New public surface: **0**.

## Files-to-delete

Test `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose`: behaviour gone, disclosed (citation → D7-1).
binding.h's "no reliable end signal", "must not destroy from inside" and "may arrive before this call returns": replaced.
