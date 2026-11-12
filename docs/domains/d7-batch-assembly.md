# Domain 7: Batch assembly

> **Specified.** The owner confirmed these laws on 2026-11-10 ([RIX-rulings.md](../../plans/RIX-rulings.md), "Yes on all accounts"). A change to a law needs
> an owner ruling, recorded in a decision of record. **Amended 2026-11-12** (D14, D15): L4 and L5
> gain the no-timeout case, L15 is new, and §1 no longer lists validating `BatchOptions` as refused;
> L15's refusal is `[built]` (D7O-1, `9abb0c7`); the no-timeout and unrepresentable-timeout cases of
> L4 and L5 are `[built]` (D7O-2); a huge `max_rows` is `[built]` (D7O-3).

**Mode: survey, verified against the tree at `9a4780b`; the §3 "Holds at" citations were refreshed at D7F-3; every other `cpp:N` citation predates D7F-1 (`a2c8b97`), D7F-2 (`dbaa2b6`) and D7F-3, so treat those as approximate; D7O-1 shifted every `cpp:N` after line 34 down by about 9, and D7O-2 those after `ArmTimer` by about 12 more** (branch `feature/row-index`). The code is
ground truth. Doc comments, READMEs and specs are claims checked against it. No spec exists for
this domain; its rules live in code comments in
[`subscriber_arrow.cpp`](../../pubsub-arrow/src/subscriber_arrow.cpp) (cited `cpp:N`) and
[`subscriber_arrow.hpp`](../../pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp)
(cited `hpp:N`). Tests are in `pubsub-arrow/tests/test_pubsub_arrow.cpp`, named without the
`SubscriberArrowBatchTest.` prefix.

## 1. Intent

**Owns:** turning a subscription's stream of rows into a sequence of columnar batches. That means:
deciding when a window closes (row count, timeout, 32-bit offset budget, close); accounting for every
row that arrives (delivered or dropped); keeping row-aligned side data (attachments) parallel to the
batch; reporting why each batch was delivered; owning the batch thread and its teardown; and deciding
whether a subscription's `BatchOptions` are acceptable (L15, since 2026-11-12).

**Refuses:**
- **Decoding.** How a wire row becomes builder contents, which schemas can be decoded, and the
  dictionary re-fold all belong to `BatchDecoder`, which is domain 2's. This domain only reacts to the
  decoder's three outcomes: appended, `invalid_argument`/`BatchCapacityExceeded` (nothing appended),
  or an internal failure (the pending window is undefined) (`batch_decoder.hpp:46-49`).
- **Delivery and subscription lifetime.** Fan-out, the unsubscribe barrier, re-entry refusal and
  delivery threading belong to domain 3. This domain relies on them (see §6).
- **Retry, back-pressure, persistence.** A lost row is counted and never recovered. A slow callback
  is not throttled. No code exists for any of these.
- ~~**Validating `BatchOptions`.** Out-of-range values are clamped silently, never refused.~~
  Withdrawn 2026-11-12 (D15): validating them is this domain's, by L15.

## 2. Ubiquitous language

| Term | Definition (as the code behaves) |
|---|---|
| **Window** | The rows and drops collected since the previous flush: `decoder_` rows + `atts_` + `dropped_` (`cpp:256-258`). |
| **Pending** | The window is non-empty: at least one decoded row or one drop (`HasPending`, `cpp:152`). |
| **Flush** | Closing the window: it is finished into a batch and queued, in window order; the subscription's batch thread delivers it (D10; `Close`, since D7F-3). |
| **Flush reason** | `BatchStatus::Reason`: `kRowLimit`, `kTimeout` or `kClosing` (`hpp:80-84`). |
| **Batch** | The `arrow::RecordBatch` a flush delivers. It may have zero rows, or be null (L6, L7). |
| **Dropped row** | A row that reached the batcher and is not in any batch. It is counted in `rows_dropped` ("rows lost since the previous flush", `hpp:86`). |
| **Deadline** | When a pending window times out: `max(now + 1 ms, last_flush + timeout)`, for a timeout the clock can reach; otherwise none (L5) (`ArmTimer`). |
| **Offset budget** | Arrow's 32-bit builder limit, which `BatchDecoder` signals with `BatchCapacityExceeded` (`batch_decoder.hpp:16-21`). |
| **Closing** | `Stop()`: the final flush. Any later row is ignored (`cpp:80`, `cpp:130-149`). |

**Naming findings:**
- *window* and *batch* both name the accumulating set. `hpp:97` says "first row/drop of the current
  batch", and `cpp:203` says "a window with only dropped rows".
- *row*, *sample* and *message* all name the unit that is dropped: "row" in `hpp:86`, "samples" in
  the README (line 86), "message" in `cpp:359`.
- `kRowLimit` has two meanings: `max_rows` was reached, **or** the offset budget was exceeded
  (`cpp:92`, `hpp:93`). One name covers two causes.

## 3. Invariants and laws

**L1. A lost row is always reported, never silent.** Every row that reaches the batcher before
closing and is not delivered is counted in the `rows_dropped` of a later delivered batch.
`[built]` (violation 1: closing D7F-1, deadline D7F-2; violation 2: D7F-3)
- Holds at: `cpp:95-99` (no decoder), `cpp:149-163` (`TryAppend`: malformed, internal),
  `cpp:100-105` (over budget: close, retry, count), `cpp:170-177` (`DiscardPending`),
  `cpp:225-226` (`Close` hands the count to the queued batch).
- **With no timeout (L5, D7O-2),** a window of only lost rows never counts toward `max_rows` (it
  reads `decoder_->num_rows()`), so it is reported only in the `kClosing` batch; the header says so, and the README points to it.
- **Violation 1.** While no usable schema exists (none decided yet, or an import that threw, which
  decides the schema as unusable for good), the row is counted through `AddRow`'s no-decoder branch. Since `a2c8b97` (D7F-1) the closing flush
  reports it; since D7F-2 the batch thread reports it at each deadline too, because `Loop` flushes at
  every deadline it reaches and the wait on `ready_` is deleted. (Before D7F-1, `NoteDropped` counted
  it and `Flush` discarded the count.)
- **Violation 2** (fixed by D7F-3). A row that hit the offset budget, during whose early flush the
  callback called `Unsubscribe`, was neither appended nor counted. Closing a window now only queues
  its batch, so the row is retried, and counted if it does not fit even alone, before any callback
  can run.
- Tests: `DroppedRowReportedAndAttachmentDiscarded`, `CorruptRowIsCountedDroppedAndBatchStaysAligned`,
  `OnlyDroppedRowsStillDeliversEmptyBatch`, `NestedDictionarySchemaReportsEveryRowDropped`,
  `FinishFailureIsReportedNotFatal`. Violation 1's closing half: the four `NoUsableSchema*` / `SchemaDeclaredAfterSubscribe*` tests (D7F-1). Its deadline half (D7F-2):
  `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles`,
  `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline`, the guard
  `EveryAssemblyStateReportsItsDropsAtEitherEnding`, and the leg
  `SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops` (drops, then a schema, in one window).
  Violation 2: `OverBudgetRowAroundInCallbackUnsubscribeIsCounted` (labelled `heavy`, D11), and the
  guard `RowsConservedAtEveryEnding`.
- **Both violations are to be fixed before RIX** (owner ruling 2026-11-10, "2. Fix both"). Violation 1
  is fixed by D7F-1 and D7F-2 (Q1 resolved); violation 2 by D7F-3.

**L2. Rows are conserved.** For each window, rows in the batch + `rows_dropped` = rows that reached
the batcher in that window, with each row counted exactly once.
`[built as a property; no code owner]`
- No single function holds this. It is the sum of each `AddRow` branch (`cpp:87-109`, with
  `TryAppend` `cpp:149-163`), of `DiscardPending` (which moves pending rows into the drop count,
  `cpp:171`), and of the reset in `Close` (`cpp:223-226`). The exceptions are L1's.
- Tests: `CorruptRowIsCountedDroppedAndBatchStaysAligned` (2 + 1), `FinishFailureIsReportedNotFatal`
  (200 dropped, then 3 delivered), `ReuseAcrossWindows`.

**L3. Side data stays row-aligned.** `attachments.size() == batch rows`, and `attachments[i]` belongs
to batch row `i`. A dropped row takes its attachment with it.
`[built]`
- Holds at: an attachment is pushed only after a successful `Append` (`cpp:151-152`);
  `DiscardPending` clears them (`cpp:172`); `Close` moves them out together with the batch
  (`cpp:223-225`). Stated at `hpp:109-114`.
- Tests: `AttachmentsAlignWithRows`, `DroppedRowReportedAndAttachmentDiscarded`,
  `CorruptRowIsCountedDroppedAndBatchStaysAligned`.

**L4. A window is bounded.** It closes at the first of four events: `max_rows` rows, the deadline
(only for a subscription that has a timeout; L5), the offset budget, or closing. The batch's reason
names which, except that the offset budget is reported as `kRowLimit`. A row that meets the budget
opens the next window rather than being lost.
`[built]`; the no-timeout case `[built]` (D14, D7O-2)
- Holds at: `cpp:108` (count), `cpp:92-94` and `cpp:251-252` (deadline), `ArmTimer` (no deadline
  when there is no timeout), `cpp:100-105` (budget: close, then retry once), `cpp:122` (closing).
- Tests: `FlushesAtRowLimit`, `FlushesAtTimeout`, `ClosingFlushOnUnsubscribe`;
  no timeout: `NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing` (with the control
  `ShortTimeoutControlSeesTheDeadlineInEveryBody`). The offset-budget
  early flush: `OverBudgetRowAroundInCallbackUnsubscribeIsCounted` (`heavy`, D7F-3), whose first
  batch is `kRowLimit` with one row under a large `max_rows`; the decoder half:
  `BatchDecoderTest.CapacityExceededLeavesBuildersUntouched`.

**L5. Timeout cadence is anchored to the previous flush, not to the first event.** A continuous
stream flushes every `timeout`. A stream idle for longer than `timeout` is delivered on the next
event, after a 1 ms floor. **A subscription may have no timeout:** its windows have no deadline and
close only on row count, the offset budget or closing (D14). **A deadline is never earlier than its
timeout says:** a timeout too long for the clock to represent means no deadline, never an early or
wrapped one.
`[built]` for finite timeouts the clock can represent; `[built]` for no timeout and for the
unrepresentable case (D14, D7O-2).
- Holds at: `ArmTimer` (the deadline, or none when the timeout cannot be reached from the previous
  flush: the sum is formed only when it can be represented), `cpp:222` (`Close` sets `last_flush_`, the anchor), `cpp:102` and `cpp:271-276` (where the deadline is read).
- Tests: no timeout (`BatchOptions::kNoTimeout`): `NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing`;
  the unrepresentable case: `TimeoutTooLongForTheClockBehavesAsNoTimeout` (its 100-year arm is a
  guard: a long timeout the clock can reach is accepted and never fires early). `FlushesAtTimeout` checks only that a
  timeout flush happens, not when.
- The header and README now state this rule (D7F-2; Findings a1).

**L6. Loss gets a delivery, and idleness does not.** A window that contains only drops is still
delivered: a zero-row batch, or a null batch if there is no decoder. A window with nothing pending
delivers nothing. With no timeout, such a window waits for closing (L1).
`[built]`
- Holds at: `cpp:201`, `cpp:145`, `cpp:204-213`.
- Tests: `OnlyDroppedRowsStillDeliversEmptyBatch`; the idle case: `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose` (D7F-1);
  at the deadline with no decoder, and the idle stretch between two such windows (nothing delivered):
  `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` (D7F-2).

**L7. A null batch means "no batch could be built for this window".** It is never "no rows".
`[built]`
- Since `a2c8b97` (D7F-1) the header and README state D9's wording, which matches the law. (Before,
  the header claimed a batch was null *only* when the schema was rejected.)
- Tests: `NestedDictionarySchemaReportsEveryRowDropped`, `FinishFailureIsReportedNotFatal`, the
  four D7F-1 no-usable-schema tests, and the D7F-2 deadline tests. A window that gains a schema before
  it closes is a normal batch carrying the earlier drops:
  `SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops`.

**L8. An undecodable schema is permanent for the subscription.** Once a schema has been rejected,
every later row is dropped, and there is no recovery.
`[built]`
- Holds at: `cpp:65-77` (`SetSchema` refuses a second decision, `cpp:67`), called from the
  subscription's lambda at `cpp:391-398`. Stated at `hpp:106-109`.
  Since D7F-2 an import failure is decided once too: a row whose schema `ImportArrowSchema` throws on
  fixes the subscription's schema as null for life, the same door and the same permanent result as a
  schema `BatchDecoder` rejects. A schema that is merely absent, or nothing to import (a released
  handle), decides nothing.
- D7O-3: the `BatchDecoder` constructor alone decides. The room set aside up front for a window
  (`PreSize`, `RecordBatchBatcher`) is a hint: it runs after the constructor, outside the verdict, and
  a failure is ignored. `SetSchema` has no path that discards a decoder once it exists (ruling
  2026-11-12, "Fix in D7O"). One residue is open: a constructor that fails on allocation, not on
  rejection, still decides "no decoder" for life (Findings (d), Q8).
- Tests: `NestedDictionarySchemaReportsEveryRowDropped` and the undecodable-schema cells of
  `EveryAssemblyStateReportsItsDropsAtEitherEnding`, which cover one window each. An import failure
  is forced by that guard's unimportable-schema cells: `Publisher::CreateTopic` accepts a schema
  whose format string Arrow rejects, and the Mock hands it on, so `ImportArrowSchema` throws. Each
  cell sends one malformed row, then declares a good schema and sends a well-formed row: the
  subscription still drops it, so the failed import was not retried. The deadline cell waits for the
  first row's `kTimeout` report, sends the second row, waits for its own `kTimeout` report, and
  gets two null batches with `rows_dropped` 1 and 1. The close cell sends both rows before
  `Unsubscribe` and gets one null `kClosing` batch at the end, with `rows_dropped` 2.
  D7O-3: `AFailedPreSizeDoesNotMakeTheSchemaUnusable` (a decodable schema whose pre-size fails still
  has a decoder: a non-null zero-row `kClosing` batch) and `MaxRowsLargerThanAnyWindowIsUsedAsGiven`.

**L9. Closing is final and reported.** The pending window is delivered once, as `kClosing`. After
`Unsubscribe` or `~SubscriberArrow` returns, no further batch is delivered. **The exceptions:**
when `Unsubscribe` is called, or the `SubscriberArrow` is destroyed, from inside the subscription's
own batch callback, or when `Unsubscribe` is called from a thread delivering rows on the
subscription's own transport, the call does not wait for that subscription's batches, and
delivery continues: any full windows already queued are delivered first, in order, then the
`kClosing` batch, and nothing after it. A destruction still waits for the object's other batched
subscriptions. The callback must outlive those batches, and the caller may not free what they use
until the `kClosing` batch has arrived. A row that arrives after closing is neither delivered nor
counted.
`[built]` (D7F-3)
- **Revised 2026-11-10** (owner ruling, "Deliver after it returns", [RIX-rulings.md](../../plans/RIX-rulings.md)).
  The law used to say only "after `Unsubscribe` returns, no further batch is delivered". That could
  not hold together with L12 and L1 when the window was non-empty (D7F pre-round audit, LL-1).
- **Clarified 2026-11-10** (owner ruling, "Queued batch, then closing"): "right after that callback
  returns" means that `kClosing` is the last delivery, after any windows already queued (D10).
- **Widened 2026-11-11** (owner rulings, Decisions 1 (a) and 2 (b), D12 and D13): the exception
  covers destroying the `SubscriberArrow` inside its own batch callback, and an `Unsubscribe` from a
  row-delivery thread on the same transport (domain 3's no-wait-in-delivery rule, extended to
  batches). Destroying it from a row-delivery thread stays forbidden (2026-09-05).
- Holds at: `cpp:116-138` (`Stop`: idempotent through `stopped_`; `kClosing` queued behind the
  queue; returns at once in the exceptions, `cpp:125`), `cpp:89` (later rows ignored),
  `cpp:247-248` (the loop ends only when closed and drained), `cpp:440-465` (`Unsubscribe`: `Stop`
  before `Subscriber::Unsubscribe`; an entry leaves `batchers_` only after its loop ended),
  `cpp:303-319` (the destructor).
- Tests: `ClosingFlushOnUnsubscribe`, `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly`,
  `UnsubscribeFromInsideATimeoutFlushIsSafe`, `NoUsableSchemaDropsReportedAtDestruction` and
  `SchemaDeclaredAfterSubscribeDropsReportedAtDestruction` (the closing flush from the destructor,
  D7F-1). D7F-3: `ClosingFromAnotherThreadDeliversQueuedWindowsThenClosing` (from outside, the
  destructor, a racing cancel) and `UnsubscribeFromInsideACallbackNeverDeliversNested` (cases (a)-(c)
  in-callback, (d) from a row-delivery thread, (e) destruction inside the callback).

**L10. Neither user code nor allocation failure can kill an internal thread.** A throw from the
callback is swallowed. An internal failure becomes a drop.
`[built]`
- Holds at: `cpp:242-245`; `cpp:159-160` (`catch (...)`), `cpp:173-176`, `cpp:206-213`.
- Tests: `FinishFailureIsReportedNotFatal` covers internal failures. A throwing callback has no test.

**L11. The batcher outlives every thread running inside it.** The batch thread co-owns the batcher.
A `Stop` issued from the batch thread detaches the thread instead of joining it.
`[built]`
- Holds at: `cpp:51-59` (`Start`'s self capture), `cpp:125-129`.
- Tests: `UnsubscribeFromInsideATimeoutFlushIsSafe`.

**L12. Batches reach the callback one at a time, in window order.**
`[built]` (D7F-3)
- Holds at: the batch thread's loop, the only call to the callback; every window closes under
  `mu_` onto one in-order queue that only that thread drains (D10).
- Fixed by D7F-3. Before, `Flush` released `mu_` around the callback, so a `kTimeout` callback on
  the timer thread could overlap with, and be overtaken by, a `kRowLimit` callback from the
  delivery thread or a `kClosing` callback from the thread calling `Unsubscribe`.
- Tests: `EveryCallbackRunsOnTheBatchThreadInWindowOrder`, the guard `DeliveryOrderStress`, and the
  in-flight and nesting checks of rungs 4 and 6.
- **This is a law, and the violation is to be fixed before RIX** (owner ruling 2026-11-10, "2. Fix
  both"). L14 depends on it.

**L13. Subscribing never waits for the schema.** Assembly starts at the first decodable row.
`[built]`
- Holds at: `cpp:391-398`, `cpp:413-418`.
- Test: `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` is the first batched
  subscriber-first test (D7F-2): it subscribes before any declaration and assembles once the schema
  arrives. `PubSubArrowFastDdsTest.SubscribeBeforePublishDeliversWithSchema` in
  `integration-tests/pubsub-arrow-fastdds` uses the per-row overload.

**L15. Out-of-range batch options are refused, never adjusted.** A batched `Subscribe` whose
`max_rows` is below 1 or whose timeout is negative is refused with an invalid-argument error before
anything is subscribed: no thread, no provider subscription, no delivery. Every value it accepts is
used as given. This domain is the one place the question "are these options acceptable?" is
answered (register BC-8); a binding passes the refusal through instead of answering it again.
`[built]` for the refusal (D15, D7O-1); the "used as given" clause is `[built]` for timeouts
(D7O-2) and for `max_rows` (D7O-3)
- Source: owner ruling, 2026-11-12, "Refuse both" ([RIX-rulings.md](../../plans/RIX-rulings.md)).
- Holds at: `RecordBatchBatcher`'s constructor (`cpp:34-47`), reached only from the batched
  `SubscriberArrow::Subscribe`, before the batch thread starts or anything is subscribed. D7O-3:
  `max_rows_` is stored and compared unchanged; the one pre-size helper (`PreSize`, bounded at
  `kMaxPreSizeRows`, 65 536 rows) sizes a buffer only, never a window, and its failure is ignored.
- Tests: `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` (rung 1: each out-of-range value
  refused, callback released, nothing subscribed or delivered); `InRangeBatchOptionsAreUsedAsGiven`
  (`max_rows` 1 and a zero timeout accepted and used as given). D7O-2: `TimeoutTooLongForTheClockBehavesAsNoTimeout`
  (`milliseconds::max()`, the clock's maximum and 100 years accepted, none firing early). D7O-3:
  `MaxRowsLargerThanAnyWindowIsUsedAsGiven` (`INT64_MAX` rows delivered, on a deadline and at
  closing; 2^17 + 1 rows fills to exactly that number) and the BC-8 sweep
  `EveryBatchOptionIsRefusedExactlyWhenOutOfRange` (42 cells, refused exactly when `max_rows < 1` or
  the timeout is negative; every accepted cell delivers its row).

**L14. The row index is always the caller's, never Fletcher's.** Batch assembly delivers each row's
index exactly as the publisher supplied it. It never assigns, renumbers, reorders, fills or checks an
index. Keeping indexes compact (no gaps) and in sequence is the caller's responsibility, and Fletcher
makes no claim about it.
`[designed-not-built]`
- Source: owner ruling, 2026-11-10: *"The row index is always supplied by the caller, never by
  fletcher itself; it is the caller's responsibility that the rows are indexed compactly and in
  sequence"* ([RIX-rulings.md](../../plans/RIX-rulings.md)). It agrees with the earlier ruling *"For
  now, we will make no counting check"*.
- **Depends on L12.** A caller's correct sequence reaches the consumer in sequence only if batches
  reach the callback in window order. While L12 is violated, batch assembly can reorder a correct
  sequence, which is exactly what this law forbids.
- Holds at: not built (RIX stage S4). Test: owed by RIX.

## 4. The interface

Surface (`hpp:72-144`):
- `BatchOptions{max_rows = 8000, timeout = 1 min}`, and `BatchOptions::kNoTimeout`
  (`milliseconds::max()`) for no timeout (D14, D7O-2). A batched `Subscribe` refuses out-of-range
  options (L15).
- `BatchStatus{reason, rows_dropped}`.
- `RecordBatchCallback(batch, attachments, status)`.
- `Subscribe(segments, RecordBatchCallback, BatchOptions, TopicOptions = {})`, plus a convenience
  overload with the default options (`hpp:139-142`).
- `Unsubscribe(id)` and `~SubscriberArrow`, which trigger the closing flush.

`RecordBatchBatcher` is private (`hpp:154`). Only `SubscriberArrow` constructs or drives it.
`TopicOptions` passes through to domain 3 untouched (`cpp:383`).

**Who calls, and on which thread:**
- Applications call `Subscribe` and `Unsubscribe` from any thread, including from inside the batch
  callback (tests above).
- The callback runs only on the subscription's own batch thread, for every reason, one batch at a
  time, in window order (D10). The provider's delivery thread only queues finished windows; it
  never runs or waits on a callback. `Unsubscribe` and the destructor queue the `kClosing` batch and
  wait for it, except in L9's exceptions, where they do not wait for that subscription's batches
  (a destruction still waits for the object's other batched subscriptions).
- The callback target must outlive the subscription (README line 90).

## 5. Decisions of record

| # | Decision | Source |
|---|---|---|
| D1 | Each window reports a reason and a drop count. A dropped row contributes neither a row nor an attachment. A window with only drops still delivers a zero-row batch. A partial window is flushed as `kClosing`. | Commit `7cd3e53` message (the domain's origin). The test comment "decision A" (test line 673) has no source in `plans/` or `docs/archive/`. |
| D2 | The timeout is anchored to the previous flush, with a 1 ms floor. | Code comment `cpp:168-172`, with a measurement (65 ms against 33 ms at 30 Hz), introduced in `f33973c`. No owner ruling was found. |
| D3 | This domain is specified, in its own survey round, before RIX. | Owner ruling, 2026-11-10, "3. (a)" ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D4 | `SubscriberArrow` is a boundary, not a domain. Batch assembly is a domain of its own. | Owner rulings, 2026-11-10 ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D5 | Re-entry from inside a delivery is refused. `Subscriber` skips provider teardown when a handler cancels its own last subscription, which is what makes `Unsubscribe` from a `kRowLimit` callback safe. | Owner rulings, 2026-09-05 ([pubsub-interface-spec.md](../pubsub-interface-spec.md) §6, clauses 5-6; `pubsub/src/subscriber.cpp:722-740`). This is domain 3's decision, relied on here. |
| D6 | L1's two violations and L12's violation are fixed in a round before RIX. | Owner ruling, 2026-11-10, "2. Fix both" ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D7 | The row index is always the caller's, never Fletcher's (L14). | Owner ruling, 2026-11-10 ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D8 | L9 revised: after an `Unsubscribe` from inside a batch callback, the closing batch is delivered once that callback returns. | Owner ruling, 2026-11-10, "Deliver after it returns" ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D9 | The header and README describe a null batch as "no batch could be built for this window: no usable schema has arrived yet, the schema was rejected (permanent), or finishing the batch failed; `rows_dropped` counts the window's rows in every case". Supersedes the header's "null only when the constructor rejected" (`hpp:106-111`). | Owner ruling, 2026-11-10, "Approve the rewording" ([RIX-rulings.md](../../plans/RIX-rulings.md)). Applied by round D7F. |
| D10 | Every batch callback runs on the subscription's own batch thread. Full windows are queued for it in order, so the transport's delivery thread never runs or waits on a callback. Accepted cost: no back-pressure on the transport, so a callback slower than the data rate makes batches queue up in memory. Applied by round D7F, which also rewrites §4 "Who calls, and on which thread". | Owner ruling, 2026-11-10, "Always Fletcher's batch thread" ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D11 | The test for L1's second violation (~256 MiB row) carries the `heavy` label and is not in the default run. | Owner ruling, 2026-11-10, "Label it 'heavy'" ([RIX-rulings.md](../../plans/RIX-rulings.md)). |
| D12 | Destroying the `SubscriberArrow` inside its own batch callback is treated like an in-callback `Unsubscribe`: queued batches, then `kClosing`, arrive after the callback returns, and the callback must outlive them (L9). | Owner ruling, 2026-11-11, Decision 1 (a) ([RIX-rulings.md](../../plans/RIX-rulings.md)). Applied by D7F-3. |
| D13 | An `Unsubscribe` from a thread delivering rows on the subscription's own transport returns at once; the remaining batches, then `kClosing`, still arrive in order on the batch thread, and the caller may not free what those callbacks use until `kClosing` has arrived (L9). | Owner ruling, 2026-11-11, Decision 2 (b) ([RIX-rulings.md](../../plans/RIX-rulings.md)). Applied by D7F-3. |
| D14 | A batched subscription may have no timeout: windows close only on row count, budget or closing. | Owner ruling, 2026-11-12, "Add 'never' to C++" ([RIX-rulings.md](../../plans/RIX-rulings.md)). Applied by round D7O. |
| D15 | Out-of-range `BatchOptions` (`max_rows < 1`, negative timeout) are refused with an invalid-argument error, never clamped (L15). Resolves Q5. | Owner ruling, 2026-11-12, "Refuse both" ([RIX-rulings.md](../../plans/RIX-rulings.md)). Applied by round D7O. |
| D16 | There is one batch assembler, this domain's. C#'s managed batcher is deleted once C# reaches this one through the shim's server-tier variant. | Owner rulings, 2026-11-12 ([RIX-rulings.md](../../plans/RIX-rulings.md)). |

## 6. Relationships

- **Domain 2 (`BatchDecoder`).** This domain consumes the decoder's exception contract
  (`batch_decoder.hpp:28-57`) and maps it onto drops (L1, L2) and early flushes (L4). Which schemas are
  rejected, and the dictionary re-fold, are domain 2's. The header (`hpp:113-122`) and the README
  restate domain 2's dictionary-shape rule. The batched path also builds a per-row `Codec` that it
  never decodes with, using it only as a schema cache (`cpp:360-368`, `cpp:410`); see Findings b2.
- **Domain 3 (`Subscriber`, providers).** This domain relies on four of domain 3's guarantees:
  delivery is serialized per subscription (spec §6.1), which makes `AddRow` single-producer;
  `Subscriber::Unsubscribe` acts as a barrier, so no delivery is in flight after it returns, which
  together with `Stop` gives L9; re-entry is handled (D5); and rows can arrive before `Subscribe`
  returns (`cpp:355-359`).
- **Domain 8 (row bookkeeping), upcoming.** [RIX-row-index.md](../../plans/RIX-row-index.md) stage S4
  extends `RecordBatchBatcher`:
  - it reads `fletcher.row_index*` at `SetSchema`;
  - it strips the reserved attachment and checks the writer hash for each row;
  - it adds a row-aligned `UInt64` index array, `BatchStatus::rows_foreign_writer` and
    `SubscribeIndexed`.

  That stage will touch L2 (a third bucket in the count) and L3 (a second row-aligned array). The
  only law written for it here is L14, which comes from an owner ruling.

## 7. Open questions

| # | Question | Owner |
|---|---|---|
| Q1 | Resolved by D7F-1 and D7F-2: those drops are reported as a null batch, at each deadline and at closing. | — |
| Q2 | Resolved: L12 is a law (2026-11-10), fixed by D7F-3 with a single batch thread (D10). | — |
| Q3 | Resolved 2026-11-10 (D9): the header was amended; a `Finish` failure keeps delivering a null batch. Applied in `a2c8b97`. | — |
| Q4 | Should the offset-budget flush get its own reason (such as `kCapacity`), rather than sharing `kRowLimit`? Adding one is a public enum change. | owner |
| Q5 | Resolved 2026-11-12 (D15, L15): `max_rows < 1` and a negative timeout are refused. A zero timeout is accepted (the 1 ms floor applies). | — |
| Q6 | Where is "decision A" (test line 673) recorded? | owner or PM |
| Q7 | Should the batched path cache an `arrow::Schema` instead of building an unused `Codec`? This touches the domain 2 boundary. | domain 2 and domain 7 |
| Q8 | Should a `BatchDecoder` constructor that fails on allocation be told apart from one that rejects the schema? Today both leave no decoder for life (L8, Findings (d)). Telling them apart needs an allocation outcome in domain 2's exception contract (`batch_decoder.hpp:28-41`). | domain 2 and domain 7 |

## 8. How to extend this domain

1. **Name the bucket.** Any new reason a row may fail to reach a batch must have a named counter in
   `BatchStatus` (`rows_dropped` today, `rows_foreign_writer` planned), so that L2 stays an equation.
   No row may leave the window uncounted.
2. **Keep side data aligned.** Any new per-row side data (index arrays, attachments) is appended in
   the same branch as the successful `Append`, cleared in `DiscardPending`, and moved out in `Flush`
   together with the batch (L3).
3. **Decide windows here, decode in domain 2.** New decoder outcomes are added to `BatchDecoder`'s
   contract first, then mapped to a drop or an early flush here.
4. **Isolate every internal thread.** User code and allocation failures stay in `try` blocks on the
   delivery, timer and teardown paths (L10, L11).
5. **Record the change.** Write any change to a law here before writing the code, and mark its build
   state.

## Findings (reported, not fixed)

**(a) Documents that disagree with the code**
1. **Timeout anchoring.** `hpp:96-97` ("since the first row/drop of the current batch"), README
   lines 66-67 ("since the batch started filling") and commit `7cd3e53` all disagree with
   `cpp:168-176` (anchored to the previous flush). See L5. **Resolved by D7F-2:** the header and
   README state L5's rule, schema or not.
2. **When a batch is null.** `hpp:106-111` ("null only when the constructor rejected") disagrees with
   `cpp:217-225`. See L7. **Resolved by D7F-1:** the header and README now state D9's wording.
3. **The codec cache's lifetime.** `cpp:345-347` says "Codecs live in `codecs_` for the
   SubscriberArrow's lifetime", but `Unsubscribe` erases them (`cpp:448-450`). Before `cpp:388`
   registers `sub_topic_`, a concurrent last-unsubscribe on the same topic can erase the entry. The
   `call_once` would then pass `SetSchema(nullptr)` (`cpp:375-379`), giving permanent drops (L1).
   This is reachable only under that race, and was found by reading, not reproduced. **Since
   `a2c8b97` (D7F-1)** those drops are reported at close. **Resolved by D7F-2:** `AcquireCodec`
   returns the entry it found or built, by value (shared owners), and the batcher is handed that
   schema; there is no second lookup, and the false lifetime comment is deleted.
4. **The wire spec.** [wire-format-specification.md:222-226](../wire-format-specification.md) says a
   nested dictionary is "rejected by that batched path at subscribe time". In the code the rejection
   happens at the first decodable row (`cpp:372`), and it is a null batch with drops (L8), not an
   error.
5. **The architecture overview.** [architecture-overview.md:188-193](../architecture-overview.md)
   names a `PubSubArrow::Subscribe`; the class is `SubscriberArrow`. It says nested value types are
   "rejected with a clear error", whereas the batched path reports drops and gives no error. It also
   omits `float16`, ordered dictionaries and dictionaries below the top level, which `BatchDecoder`
   also rejects (`batch_decoder.hpp:28-31`).
6. **The README.** The README (lines 146-147) also omits `float16` and ordered dictionaries from its
   rejected list.

**(b) Rules answered in two places**
1. **Dropping when there is no decoder.** Two paths do it, `NoteDropped` (`cpp:121-126`, no codec)
   and `AddRow` (`cpp:81-85`, no decoder), and they report differently: the first is never flushed
   (L1). **Resolved by D7F-1:** `NoteDropped` is deleted and `AddRow` is the one entry.
2. **Whether a schema is usable.** This is decided twice: `ImportArrowSchema` in `AcquireCodec`
   (`cpp:403-409`) and the `BatchDecoder` constructor (`cpp:62`). Since `a2c8b97` (D7F-1) both
   failures are reported; the single decision is still open (BC-3, Q7). **Noted by D7F-2:** the two
   decisions now share one door (`SetSchema`, once per subscription) and both are permanent; an
   import failure is no longer retried on every row.
3. **The append-and-classify block.** It is duplicated for the retry after the budget is exceeded
   (`cpp:86-112` and `cpp:96-104`). **Resolved by D7F-3:** one `TryAppend`, called twice.
4. **The dictionary-shape fallback rule.** It is stated three times: `hpp:113-122`, README lines
   100-112, and `batch_decoder.hpp:28-40`. It belongs to domain 2.
5. **Dead branch.** The `kRowLimit` flush in `SetSchema` (`cpp:69-71`) is unreachable: the decoder
   is brand new there, so it holds 0 rows, and `max_rows` is at least 1. **Resolved by D7F-1:** the
   branch is deleted.

**(c) Behaviour with no test**
- ~~The early flush on the offset budget, at the batcher level (L4).~~ Tested by D7F-3 (`heavy`).
- Timeout anchoring and the 1 ms floor (L5). (No timeout and the unrepresentable timeout: tested by
  D7O-2.)
- No delivery for an idle window (L6).
- ~~The uncounted row after the budget retry (L1, violation 2).~~ Tested by D7F-3 (`heavy`).
  Pre-schema drops are tested at close (D7F-1) and at the deadline (D7F-2).
- The closing flush from `~SubscriberArrow` (L9): tested since D7F-1 (the two `*AtDestruction` tests).
- A callback that throws (L10).
- ~~Overlapping or reordered callbacks (L12).~~ Tested by D7F-3.
- ~~Clamping of `max_rows` and `timeout` (§1): replaced by refusal (L15).~~ Resolved for the refusal
  by D7O-1 (tests under L15); huge timeouts by D7O-2 (tests under L5); a huge `max_rows` by D7O-3
  (tests under L15 and L8).

**(d) Reported at D7O-3, not fixed**
1. **A constructor that fails on allocation makes a good schema unusable for life.** `SetSchema`'s
   `catch` around `std::make_unique<BatchDecoder>` treats every constructor failure as a rejection
   (L8). A failure to allocate while building the decoder is not a rejection, but decides "no
   decoder" the same way. Fixing it needs domain 2's exception contract to name an allocation
   outcome (`batch_decoder.hpp:28-41` names none). Open question Q8; register BC-3 (Registered gap).
2. **For domain 2, noted not edited:** `arrow-bridge/benchmarks/bench_arrow_codec.cpp:388-390`
   describes `Reserve(max_rows_)` in `RecordBatchBatcher::Flush`. It has been stale since D7F renamed
   `Flush` to `Close`, and the pre-size is now `min(max_rows_, 65 536)`; it stays literally true for
   the benchmark's N of 8000 or less. Also: a failed `Reserve` can leave a builder with its data
   buffer grown and its validity bitmap not (Arrow records the capacity before the bitmap
   allocation fails). Under the 65 536-row bound only memory exhaustion between two small
   allocations reaches it, as in every `Append`'s own growth, so `Reserve`'s failure is treated as
   leaving the decoder usable, but `BatchDecoder`'s contract does not say so.
