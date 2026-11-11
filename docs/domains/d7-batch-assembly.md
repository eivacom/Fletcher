# Domain 7: Batch assembly

> **Specified.** The owner confirmed these laws on 2026-11-10 ([RIX-rulings.md](../../plans/RIX-rulings.md), "Yes on all accounts"). A change to a law needs
> an owner ruling, recorded in a decision of record.

**Mode: survey, verified against the tree at `9a4780b`; `cpp:N` citations predate D7F-1 (`a2c8b97`) and D7F-2 (`dbaa2b6`); `subscriber_arrow.cpp` has shrunk by about 30 lines since, so treat them as approximate** (branch `feature/row-index`). The code is
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
batch; reporting why each batch was delivered; and owning the timer thread and its teardown.

**Refuses:**
- **Decoding.** How a wire row becomes builder contents, which schemas can be decoded, and the
  dictionary re-fold all belong to `BatchDecoder`, which is domain 2's. This domain only reacts to the
  decoder's three outcomes: appended, `invalid_argument`/`BatchCapacityExceeded` (nothing appended),
  or an internal failure (the pending window is undefined) (`batch_decoder.hpp:46-49`).
- **Delivery and subscription lifetime.** Fan-out, the unsubscribe barrier, re-entry refusal and
  delivery threading belong to domain 3. This domain relies on them (see §6).
- **Retry, back-pressure, persistence.** A lost row is counted and never recovered. A slow callback
  is not throttled. No code exists for any of these.
- **Validating `BatchOptions`.** Out-of-range values are clamped silently, never refused
  (`cpp:32`, `cpp:176`).

## 2. Ubiquitous language

| Term | Definition (as the code behaves) |
|---|---|
| **Window** | The rows and drops collected since the previous flush: `decoder_` rows + `atts_` + `dropped_` (`cpp:256-258`). |
| **Pending** | The window is non-empty: at least one decoded row or one drop (`HasPending`, `cpp:152`). |
| **Flush** | Closing the window: it is finished into a batch and handed to the callback with the lock released (`cpp:207-248`). |
| **Flush reason** | `BatchStatus::Reason`: `kRowLimit`, `kTimeout` or `kClosing` (`hpp:80-84`). |
| **Batch** | The `arrow::RecordBatch` a flush delivers. It may have zero rows, or be null (L6, L7). |
| **Dropped row** | A row that reached the batcher and is not in any batch. It is counted in `rows_dropped` ("rows lost since the previous flush", `hpp:86`). |
| **Deadline** | When a pending window times out: `max(now + 1 ms, last_flush + timeout)` (`cpp:176`). |
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
`[built for decode failures, internal failures and undecodable schemas; violation 1 built (closing:
D7F-1; deadline: D7F-2); violation 2 open (D7F-3)]`
- Holds at: `cpp:81-85` (no decoder), `cpp:100-111` (malformed, over budget, internal),
  `cpp:159-166` (`DiscardPending`), `cpp:235-236`.
- **Violation 1.** While no usable schema exists (none decided yet, or an import that threw, which
  decides the schema as unusable for good), the row is counted through `AddRow`'s no-decoder branch. Since `a2c8b97` (D7F-1) the closing flush
  reports it; since D7F-2 the timer reports it at each deadline too, because `TimerLoop` flushes at
  every deadline it reaches and the wait on `ready_` is deleted. (Before D7F-1, `NoteDropped` counted
  it and `Flush` discarded the count.)
- **Violation 2.** A row that hit the offset budget, during whose early flush the callback called
  `Unsubscribe`, is neither appended nor counted (`cpp:95`).
- Tests: `DroppedRowReportedAndAttachmentDiscarded`, `CorruptRowIsCountedDroppedAndBatchStaysAligned`,
  `OnlyDroppedRowsStillDeliversEmptyBatch`, `NestedDictionarySchemaReportsEveryRowDropped`,
  `FinishFailureIsReportedNotFatal`. Violation 1's closing half: the four `NoUsableSchema*` / `SchemaDeclaredAfterSubscribe*` tests (D7F-1). Its deadline half (D7F-2):
  `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles`,
  `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline`, the guard
  `EveryAssemblyStateReportsItsDropsAtEitherEnding`, and the leg
  `SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops` (drops, then a schema, in one window).
  Violation 2 has no test yet.
- **Both violations are to be fixed before RIX** (owner ruling 2026-11-10, "2. Fix both"). Violation 1
  is fixed by D7F-1 and D7F-2 (Q1 resolved); violation 2 is D7F-3's.

**L2. Rows are conserved.** For each window, rows in the batch + `rows_dropped` = rows that reached
the batcher in that window, with each row counted exactly once.
`[built as a property; no code owner]`
- No single function holds this. It is the sum of each `AddRow` branch (`cpp:86-112`), of
  `DiscardPending` (which moves pending rows into the drop count, `cpp:160`), and of the reset in
  `Flush` (`cpp:233-236`). The exceptions are L1's.
- Tests: `CorruptRowIsCountedDroppedAndBatchStaysAligned` (2 + 1), `FinishFailureIsReportedNotFatal`
  (200 dropped, then 3 delivered), `ReuseAcrossWindows`.

**L3. Side data stays row-aligned.** `attachments.size() == batch rows`, and `attachments[i]` belongs
to batch row `i`. A dropped row takes its attachment with it.
`[built]`
- Holds at: an attachment is pushed only after a successful `Append` (`cpp:87-88`, `cpp:97-98`);
  `DiscardPending` clears them (`cpp:161`); `Flush` moves them out together with the batch
  (`cpp:233`). Stated at `hpp:100-104`.
- Tests: `AttachmentsAlignWithRows`, `DroppedRowReportedAndAttachmentDiscarded`,
  `CorruptRowIsCountedDroppedAndBatchStaysAligned`.

**L4. A window is bounded.** It closes at the first of four events: `max_rows` rows, the deadline,
the offset budget, or closing. The batch's reason names which, except that the offset budget is
reported as `kRowLimit`. A row that meets the budget opens the next window rather than being lost.
`[built]`
- Holds at: `cpp:114-116` (count), `cpp:191-195` (deadline), `cpp:89-98` (budget: flush, then retry
  once), `cpp:136` (closing). `max_rows < 1` is clamped to 1 (`cpp:32`).
- Tests: `FlushesAtRowLimit`, `FlushesAtTimeout`, `ClosingFlushOnUnsubscribe`. There is **no test**
  of the offset-budget early flush in this domain: the comment at test line 936 says it cannot be
  forced. Only the decoder half is tested (`BatchDecoderTest.CapacityExceededLeavesBuildersUntouched`).

**L5. Timeout cadence is anchored to the previous flush, not to the first event.** A continuous
stream flushes every `timeout`. A stream idle for longer than `timeout` is delivered on the next
event, after a 1 ms floor.
`[built]`
- Holds at: `cpp:168-180`, `cpp:209`, `cpp:261`.
- Test: no test. `FlushesAtTimeout` checks only that a timeout flush happens, not when.
- The header and README now state this rule (D7F-2; Findings a1).

**L6. Loss gets a delivery, and idleness does not.** A window that contains only drops is still
delivered: a zero-row batch, or a null batch if there is no decoder. A window with nothing pending
delivers nothing.
`[built]`
- Holds at: `cpp:211`, `cpp:152`, `cpp:203-206`.
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
- Holds at: `cpp:58-67`, plus the one-shot `call_once` at `cpp:372`. Stated at `hpp:106-109`.
  Since D7F-2 an import failure is decided once too: a row whose schema `ImportArrowSchema` throws on
  fixes the subscription's schema as null for life, the same door and the same permanent result as a
  schema `BatchDecoder` rejects. A schema that is merely absent, or nothing to import (a released
  handle), decides nothing.
- Tests: `NestedDictionarySchemaReportsEveryRowDropped` and the undecodable-schema cells of
  `EveryAssemblyStateReportsItsDropsAtEitherEnding`, which cover one window each. An import failure
  is forced by that guard's unimportable-schema cells: `Publisher::CreateTopic` accepts a schema
  whose format string Arrow rejects, and the Mock hands it on, so `ImportArrowSchema` throws. Each
  cell sends one malformed row, then declares a good schema and sends a well-formed row: the
  subscription still drops it, so the failed import was not retried. The deadline cell waits for the
  first row's `kTimeout` report, sends the second row, waits for its own `kTimeout` report, and
  gets two null batches with `rows_dropped` 1 and 1. The close cell sends both rows before
  `Unsubscribe` and gets one null `kClosing` batch at the end, with `rows_dropped` 2.

**L9. Closing is final and reported.** The pending window is delivered once, as `kClosing`. After
`Unsubscribe` or `~SubscriberArrow` returns, no further batch is delivered. **The one exception:**
when `Unsubscribe` is called from inside a batch callback, delivery continues after that callback
returns: any full windows already queued are delivered first, in order, then the `kClosing` batch,
and nothing after it. A row that arrives after closing is
neither delivered nor counted.
`[built for Unsubscribe outside a callback; violated inside one: today the kClosing batch is delivered nested, inside the calling callback]`
- **Revised 2026-11-10** (owner ruling, "Deliver after it returns", [RIX-rulings.md](../../plans/RIX-rulings.md)).
  The law used to say only "after `Unsubscribe` returns, no further batch is delivered". That could
  not hold together with L12 and L1 when the window was non-empty (D7F pre-round audit, LL-1).
- **Clarified 2026-11-10** (owner ruling, "Queued batch, then closing"): "right after that callback
  returns" means that `kClosing` is the last delivery, after any windows already queued (D10).
- Holds at: `cpp:130-149` (idempotent through `stopped_`), `cpp:80` and `cpp:123`, `cpp:428-429`
  (`Stop` before `Subscriber::Unsubscribe`), `cpp:275-288`.
- Tests: `ClosingFlushOnUnsubscribe`, `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly`,
  `UnsubscribeFromInsideATimeoutFlushIsSafe`, `NoUsableSchemaDropsReportedAtDestruction` and
  `SchemaDeclaredAfterSubscribeDropsReportedAtDestruction` (the closing flush from the destructor,
  D7F-1).

**L10. Neither user code nor allocation failure can kill an internal thread.** A throw from the
callback is swallowed. An internal failure becomes a drop.
`[built]`
- Holds at: `cpp:243-246`; `cpp:104` and `cpp:111` (`catch (...)`), `cpp:162-165`, `cpp:228-231`.
- Tests: `FinishFailureIsReportedNotFatal` covers internal failures. A throwing callback has no test.

**L11. The batcher outlives every thread running inside it.** The timer thread co-owns the batcher.
A `Stop` issued from the timer thread detaches the thread instead of joining it.
`[built]`
- Holds at: `cpp:39-51`, `cpp:141-148`.
- Tests: `UnsubscribeFromInsideATimeoutFlushIsSafe`.

**L12. Batches reach the callback one at a time, in window order.**
`[built for one flushing thread; violated on timer and delivery-thread overlap]`
- `Flush` releases `mu_` around the callback (`cpp:238-247`). A `kTimeout` callback running on the
  timer thread can therefore overlap with, and be overtaken by, a `kRowLimit` callback from the
  delivery thread, or a `kClosing` callback from the thread calling `Unsubscribe`. Commit `7cd3e53`
  claims that "provider callbacks and the timer serialize through the batcher's mutex": that holds
  for the window state, but not for the callback.
- Test: no test.
- **This is a law, and the violation is to be fixed before RIX** (owner ruling 2026-11-10, "2. Fix
  both"). L14 depends on it.

**L13. Subscribing never waits for the schema.** Assembly starts at the first decodable row.
`[built]`
- Holds at: `cpp:369-380`, `cpp:391-395`.
- Test: `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` is the first batched
  subscriber-first test (D7F-2): it subscribes before any declaration and assembles once the schema
  arrives. `PubSubArrowFastDdsTest.SubscribeBeforePublishDeliversWithSchema` in
  `integration-tests/pubsub-arrow-fastdds` uses the per-row overload.

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
- `BatchOptions{max_rows = 8000, timeout = 1 min}`.
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
- The callback runs on the provider's delivery thread for `kRowLimit` (`cpp:115`), on the batcher's
  timer thread for `kTimeout` (`cpp:195`), and on the thread calling `Unsubscribe` or the destructor
  for `kClosing`. That last thread is the timer thread when `Unsubscribe` is called from a timeout
  callback.
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
| Q2 | L12: resolved 2026-11-10. It is a law, and it is to be fixed. A delivery lock versus a single flushing thread is a design choice for the fix round. | design of the fix round |
| Q3 | Resolved 2026-11-10 (D9): the header was amended; a `Finish` failure keeps delivering a null batch. Applied in `a2c8b97`. | — |
| Q4 | Should the offset-budget flush get its own reason (such as `kCapacity`), rather than sharing `kRowLimit`? Adding one is a public enum change. | owner |
| Q5 | Should `BatchOptions` refuse `max_rows < 1` and `timeout <= 0` instead of clamping them silently? | owner |
| Q6 | Where is "decision A" (test line 673) recorded? | owner or PM |
| Q7 | Should the batched path cache an `arrow::Schema` instead of building an unused `Codec`? This touches the domain 2 boundary. | domain 2 and domain 7 |

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
   (`cpp:86-112` and `cpp:96-104`).
4. **The dictionary-shape fallback rule.** It is stated three times: `hpp:113-122`, README lines
   100-112, and `batch_decoder.hpp:28-40`. It belongs to domain 2.
5. **Dead branch.** The `kRowLimit` flush in `SetSchema` (`cpp:69-71`) is unreachable: the decoder
   is brand new there, so it holds 0 rows, and `max_rows` is at least 1. **Resolved by D7F-1:** the
   branch is deleted.

**(c) Behaviour with no test**
- The early flush on the offset budget, at the batcher level (L4).
- Timeout anchoring and the 1 ms floor (L5).
- No delivery for an idle window (L6).
- The uncounted row after the budget retry (L1, violation 2). Pre-schema drops are tested at close
  (D7F-1) and at the deadline (D7F-2).
- The closing flush from `~SubscriberArrow` (L9): tested since D7F-1 (the two `*AtDestruction` tests).
- A callback that throws (L10).
- Overlapping or reordered callbacks (L12).
- Clamping of `max_rows` and `timeout` (§1).
