# D7F-3 — One batch thread, in order, nothing lost at close (design)

Round D7F, item D7F-3, ladder rungs 3–6 (one stage by standing decision). Branch
`feature/d7f-batch-assembly`, HEAD `9ab5d86`. Citations: `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`,
`hpp` = its header, `test` = `pubsub-arrow/tests/test_pubsub_arrow.cpp`, `d7` =
`docs/domains/d7-batch-assembly.md`, `reg` = `docs/domains/bounded-context-register.md`, `sub` =
`pubsub/src/subscriber.cpp`. Line numbers are at `9ab5d86` (the ladder's are stale; audit 0a).

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **169 lines, 14 entries**; the last is
"2026-11-10 — Queued windows are delivered before the closing batch".

## Summary

Every batch callback moves onto the subscription's own batch thread (the existing timer thread).
Closing a window only queues its batch; the batch thread is the one place that calls the callback,
one batch at a time, in window order (L12, D10). Closing from another thread waits until the queue
and the `kClosing` batch are delivered (L9); closing from inside a callback returns at once and the
batch thread delivers the rest after the callback returns (L9 as revised). The over-budget row is
retried under the lock, so it is always counted (L1 violation 2). DEBT-6 is settled: the batcher
owns "has the schema been decided". Net production lines ≈ +30.

## Design

### 1. Close queues; the batch loop delivers (C1, BC-1, D-D7F-7)

- **`Close(reason)`** (today's `Flush` minus its delivery half, cpp:189-229) is the **one window
  closer**. Caller holds `mu_`. It resets the deadline and `last_flush_`, returns if nothing is
  pending (L6), finishes the batch exactly as today (null batch without a decoder, `DiscardPending`
  on a `Finish` failure, `Reserve`), and pushes `{batch, atts, BatchStatus{reason, dropped}}` onto
  `std::deque<Delivery> queue_`, then notifies `cv_`. It never releases `mu_`.
- **The batch loop** (replaces `TimerLoop`, cpp:167-183; same thread, started by `Start`) is the
  **only call to `cb_`**. Under `mu_` it records its own thread id once (`loop_id_`), then repeats:
  1. queue non-empty → pop the front, release `mu_`, `try { cb_(...) } catch (...) {}` (L10), relock;
  2. else `stopped_` → leave the loop (Stop has already closed the last window, §3);
  3. else a reached deadline → `Close(kTimeout)` (L5's arithmetic in `ArmTimer` is untouched);
  4. else wait on `cv_` (until the deadline if one is armed). Spurious wakeups only re-run the checks.
  On leaving, it sets `ended_ = true` under `mu_` and notifies.
- Three triggers call the one closer: `AddRow` (`kRowLimit`, count or budget), the loop
  (`kTimeout`), `Stop` (`kClosing`). Windows close in arrival order under `mu_` and the queue is FIFO
  with one consumer, so **delivery order = window order and in-flight ≤ 1 by construction** (L12).
- **No bound, no blocking producer** (D-D7F-2): `AddRow` pushes and returns. A slow callback grows
  `queue_`; that is the owner's accepted cost.

### 2. AddRow never delivers; one classify-and-count path (C2, BC-2, L1 violation 2, L4)

`AddRow` (cpp:72-111) keeps its door (`stopped_` → ignore, L9; no decoder → `++dropped_`, L1). The
append-and-classify block exists once, as a private `TryAppend(data, len, att)`:
`Append` + push attachment → appended; `invalid_argument` → `++dropped_`; `catch (...)` →
`DiscardPending(1)`; `BatchCapacityExceeded` → returns *budget* with nothing appended. `AddRow`:

```
if (TryAppend(...) == kBudget) {      // the window cannot take this row (L4)
    Close(kRowLimit);                 // only queues; mu_ is never released here
    if (TryAppend(...) == kBudget) ++dropped_;   // the row alone exceeds the budget
}
ArmTimer();
if (decoder_->num_rows() >= max_rows_) Close(kRowLimit);
```

Because `Close` no longer releases `mu_`, nothing can run `Stop` between the early close and the
retry. The guard `if (stopped_) return;` (cpp:87-89) is **deleted**: it was the silent loss (L1
violation 2), and it is now unreachable. The duplicate block (cpp:90-99) is deleted (census C2).

### 3. Stop: one closing path, two callers (C5, BC-5, L9, L11)

```
bool Stop() {                              // idempotent; true = every delivery has been made
  std::thread t;
  { unique_lock lk(mu_);
    if (!stopped_) { stopped_ = true; Close(kClosing); cv_.notify_all(); }  // behind the queue
    if (this_thread::get_id() == loop_id_) {           // from inside this subscription's callback
      t = std::move(thread_); lk.unlock(); if (t.joinable()) t.detach(); return false; }   // L11
    cv_.wait(lk, [&]{ return ended_.load(); });       // every queued window, then kClosing
    t = std::move(thread_); }
  if (t.joinable()) t.join();
  return true;
}
```

- **From another thread** (rung 4): the last window is closed as `kClosing` behind anything queued,
  in the same critical section that sets `stopped_`, so no row can slip between them. The caller
  then waits for the loop to end. Nothing is delivered after `Stop` returns.
- **From inside the callback** (rung 6): no delivery happens on this path at all, so nothing is
  nested. `Stop` returns; after the callback returns the loop delivers the queue, then `kClosing`,
  then ends. The detach is today's L11 behaviour; the self-owning capture in `Start` is unchanged.
- `ended_` is `std::atomic<bool>`, **initialised true** and set false in `Start` just before the
  thread is created (reset to true if creation throws), so a batcher whose thread never started
  cannot make `Stop` wait forever. `~RecordBatchBatcher` stays `Stop()` (plus the detach above when
  the last owner is the batch thread itself, exactly today's case).
- "Is this thread inside this batcher's callback?" (C6, BC-6) is answered by `loop_id_`, since under
  D10 the callback only ever runs on the loop thread. **No `DeliveryScope` token and no
  `thread_local`** are added; BC-6's single site is not duplicated.

### 4. SubscriberArrow waits for every batch thread it started (L9, ownership rule)

An in-callback `Unsubscribe` returns before its subscription's deliveries end. If `batchers_` then
forgot the batcher, a later `~SubscriberArrow` on another thread could return while that batch
thread is still calling the callback: a delivery after the destructor returned, into a callback
target the caller may already have freed. Made unrepresentable:

- `Unsubscribe(id)`: take the batcher out of `batchers_` under `mu_` (as today); call `Stop()`
  **outside** `mu_`; if it returns false (own callback), put it back under the same id. Under `mu_`,
  also move out every entry whose `Ended()` (a lock-free read of `ended_`) is true, and drop them
  after the lock is released. Then `subscriber_->Unsubscribe(id)` and the codec clean-up, unchanged.
- `~SubscriberArrow` already stops every entry of `batchers_` (cpp:255-262); it now thereby waits for
  draining ones too. A duplicate `Unsubscribe(id)` from another thread finds the draining entry and
  waits for it, like domain 3's duplicate cancel (sub:660-665).
- The `batchers_` comment (hpp:171) becomes "batched subscriptions whose deliveries have not ended".
  No member is added; the header changes in comments only.

### 5. DEBT-6: the batcher owns the schema decision (C3, BC-3)

`std::atomic<bool> decided_` in the batcher, written only in `SetSchema` under `mu_`. `SetSchema`
**refuses a second decision** (`if (decided_) return;` before anything else), so the first decision
wins whatever its caller does (L8). `bool SchemaDecided() const` reads it lock-free. The batched
lambda (cpp:324-351) loses `schema_set` (`std::once_flag`) and `schema_decided`; it becomes
`if (!batcher->SchemaDecided()) { found = AcquireCodec(...); if (found.arrow_schema ||
found.import_failed) batcher->SetSchema(...); } batcher->AddRow(...);`. Two facts, one site each:
*decided?* `decided_`; *can rows be assembled?* decoder presence.

### 6. Lock order and waits (C5)

- Batcher `mu_` is never held across `cb_`, a join, a `cv_` wait other than the loop's own and
  `Stop`'s `ended_` wait (both release it), `Subscriber::Unsubscribe`, or `SubscriberArrow::mu_`.
- `SubscriberArrow::mu_` is never held across `Stop` or a batcher destruction. `Ended()` and
  `SchemaDecided()` are atomic reads, so no lock nests in either direction.
- The transport's delivery thread holds the provider's lock (InProcess) and takes batcher `mu_`
  briefly in `AddRow`/`SetSchema`; nothing holding batcher `mu_` waits for anything, so **row
  delivery never waits on a callback** (D10). The in-callback `subscriber_->Unsubscribe(id)` now
  runs on the batch thread, outside any delivery frame, so it takes domain 3's barrier (sub:247),
  which waits only for row deliveries, and those never wait on the batch thread (premise P1).

### 7. Documentation (not counted as production)

- **hpp**: beside `RecordBatchCallback` (hpp:89-100), one paragraph: the callback runs on a thread
  the subscription owns, one batch at a time, in window order, never inside `Publish` or the
  transport's thread; and a doc comment on `Unsubscribe` (hpp:147): it delivers every finished
  window, then `kClosing`, and returns once they are delivered; called from inside this
  subscription's callback it returns at once and they follow once the callback returns.
- **README:93**: the same two sentences in the README's words.
- **d7**: §4 "Who calls, and on which thread" rewritten for D10 (the ladder's charge); §2 **Flush**
  becomes "the window is finished into a batch and queued; the batch thread delivers it";
  L1 → violation 2 built (D7F-3); L9 and L12 → `[built]` with their tests; L4 gains the heavy test
  (the first batcher-level budget test); L11's holds-at; Q2 resolved (one delivery thread); Findings
  b3 resolved; (c) budget flush, uncounted row and overlapping callbacks now tested.
- **reg**: BC-1, BC-2 `[built]`; BC-3's decided-site moves to the batcher (DEBT-6); BC-5 batch side
  `[built]`, plus the residue in Risks; BC-6: "answered by the loop's thread identity; no
  `DeliveryScope`" (the ladder left this to the architect).
- **Ladder**: drop `ReuseAcrossWindows` from rung 3's re-anchor list (audit 0a: listed in error).

## Corner cases forbidden

Rung 1, unrepresentable:
- **Two callbacks at once, or out of window order** (L12): one consumer thread, one FIFO filled under
  `mu_`.
- **A callback on the publishing/transport thread; `Publish` waiting on a callback** (D10): `AddRow`
  has no path to `cb_` and no wait.
- **The over-budget row lost around an `Unsubscribe`** (L1 v2): the early close does not release
  `mu_`, so `Stop` cannot interleave; the guard that dropped the row is deleted.
- **`kClosing` before a queued window, or anything after `kClosing`** (L9): `Stop` closes the last
  window behind the queue in the critical section that sets `stopped_`; `AddRow` ignores later rows;
  the loop ends only with `stopped_` and an empty queue.
- **A nested delivery from an in-callback `Unsubscribe`/destructor**: `Stop` contains no delivery.
- **A delivery after `~SubscriberArrow` returns on another thread, including for a subscription
  unsubscribed from its own callback**: §4's ownership rule.
- **A second schema decision** (DEBT-6, L8): `decided_` under `mu_`.
- **`Stop` waiting on a thread that never started**: `ended_` starts true.

Rung 2, refused at the door: `SetSchema` after a decision returns without effect (internal door; the
only caller is the lambda).

Handled residue:
- **A callback slower than the data rate queues batches without bound.** *Why not forbidden?* The
  owner ruled no back-pressure (D10, D-D7F-2).
- **A deadline that passes while a callback runs closes when the callback returns**, so that window
  may hold rows past its timeout. *Why not forbidden?* Closing on time needs a second clock-watching
  thread or a clock read per row on the transport's thread (D10 hot path). Nothing is lost; today a
  slow `kTimeout` callback already delays the next deadline the same way; L5 has no test.
- **In-callback `Unsubscribe` lets delivery continue after it returns.** *Why not forbidden?* Ruled
  ("Deliver after it returns", "Queued batch, then closing").

## Premises and stop conditions

- **P1.** `Subscriber::Unsubscribe` from a thread outside any delivery takes the gate barrier, which
  waits only for that subscription's in-flight row deliveries, and `provider->Unsubscribe` waits only
  for its own deliveries (sub:212-249, 641-751, read). *If either can wait on something the batch
  thread holds, STOP-AND-ASK: it is a domain 3 change.*
- **P2.** `InProcessPubSubProvider` delivers synchronously inside `Publish` under its instance mutex
  (ladder ground rules). Rungs 3–6 depend on it to show `Publish` returning while a callback blocks.
  *If false, STOP-AND-ASK; do not switch providers silently.*
- **P3.** After `BatchCapacityExceeded`, `BatchDecoder`'s builders are untouched
  (`batch_decoder.hpp:46-49`), so the retry under `mu_` is sound, as today.
- **P4.** The rung-5 crafted row (list count 2^31−1, all-null bitfield) throws `BatchCapacityExceeded`
  even into an empty window (ladder rung 5). *If it does not, STOP-AND-ASK; do not substitute a
  row-limit flush, which is what made `UnsubscribeFromCallbackDuringRowLimitFlush…` never reach the
  guard.*
- **P5.** The installed Conan has `CMake.ctest(cli_args=...)`. *If not, use
  `self.run("ctest -LE heavy …", env="conanrun")`. Never disable or skip the test (D-D7F-4).*

## Forcing-test mapping

All external: `PublisherArrow` + `SubscriberArrow` on `InProcessPubSubProvider` unless stated. The
callback records `(reason, first x, num_rows, rows_dropped, thread id)`, an atomic in-flight count
and a per-thread nesting depth; every latch wait is bounded (≤ 500 ms–2 s) so red fails, not hangs.
"Then nothing" is read after `~SubscriberArrow`, which §4 makes a complete barrier. Test names are
proposals; the PM decides.

| Test | Kind | Asserts | Red on `9ab5d86`, for the right reason |
|---|---|---|---|
| `EveryCallbackRunsOnTheBatchThreadInWindowOrder` | forcing, rung 3 | `max_rows` 2. Callback signals entered, then waits for "all publishes returned". A publisher thread publishes 6 rows. Every `Publish` returns while the first callback blocks; callback thread ≠ publisher thread, one id for all; in-flight ≤ 1; first x = 0, 2, 4. | The first callback runs inside `Publish`; its wait times out. |
| `ClosingFromAnotherThreadDeliversQueuedWindowsThenClosing` | forcing, rung 4 | (a) A `kRowLimit` callback is held while 2 more full windows queue and 1 row is pending; main `Unsubscribe`s, a helper releases the latch. Deliveries `kRowLimit`×3 then `kClosing`(1), all on the batch thread, all before `Unsubscribe` returns. (b) Same, ended by `~SubscriberArrow`. (c) Same as rung 6(c), then `~SubscriberArrow` from main while the in-callback-unsubscribed subscription still has a queued window: the destructor returns only after its `kClosing`. | `kClosing` runs on the `Unsubscribe` thread, overlapping the held callback (in-flight 2). |
| `OverBudgetRowAroundInCallbackUnsubscribeIsCounted` | forcing, rung 5, **`heavy`**, own executable | `list<int8>` schema, large `max_rows`. One good row, then `PublishDirect` of the crafted row. First delivery: `kRowLimit`, 1 row (the budget path ran). Its callback `Unsubscribe`s. Then `kClosing`, 0 rows, `rows_dropped` 1. Σ rows + Σ dropped = 2. | Σ = 1: the guard drops the row uncounted. |
| `UnsubscribeFromInsideACallbackNeverDeliversNested` | forcing, rung 6 | (a) a `kTimeout` callback blocks while another thread publishes k < `max_rows` rows, then `Unsubscribe`s (returns inside the callback) and returns: depth ≤ 1, then `kClosing` with k rows, then nothing. (b) same from a `kRowLimit` callback. (c) a held `kRowLimit` callback while another full window queues and one row is pending, then `Unsubscribe`: queued `kRowLimit`, `kClosing`(1), nothing. | `kClosing` is delivered inside the calling callback: depth 2. |
| `InCallbackUnsubscribeWhilePublishingDoesNotDeadlock` | guard (ladder; BC-5) | A callback `Unsubscribe`s while another thread is inside `Publish` holding the in-process mutex; both return within the ctest TIMEOUT. | Declared green before and after. |
| `DeliveryOrderStress` | guard (BC-1) | Fixed seeds; `max_rows` 1–3, `timeout` 1 ms; `Unsubscribe` from inside or outside at a seeded point. In-flight ≤ 1, one thread, first x strictly increasing, `kClosing` last. | Red: overlap or a second thread. |
| `RowsConservedAtEveryEnding` | guard (BC-2) | Fixed seeds; good, malformed and schema-less rows; ended outside, or inside on the first delivery after the publisher has returned from its last `Publish`. Σ rows + Σ dropped = rows sent. | Declared green before (violation 2 needs the heavy row). |

**Re-anchored** (audit 0a; subjects kept, reads moved to `sink.WaitFor` / a latch / after
`~SubscriberArrow`): `FlushesAtRowLimit` :531 (delete comment :545), `AttachmentsAlignWithRows` :599,
`DictionaryColumnRefoldedToDictionaryArray` :1106, `DictionaryColumnPreservesNulls` :1146,
`CorruptRowIsCountedDroppedAndBatchStaysAligned` :1180 (comment :1223), `FixedSizeListWithNamedItem
ArrivesNonNull` :1250, `FinishFailureIsReportedNotFatal` :1313 (first read only),
`BatchesAreValidArrow` :1446; `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly`
:1366: the callback signals after its `Unsubscribe`, main waits, publishes rows 2–3, destroys `sub`,
then asserts 1 delivery; comments :1355-1365 and :1381-1382 rewritten without the guard;
`UnsubscribeFromInsideATimeoutFlushIsSafe` :1404: the `exchange` guard becomes
`EXPECT_FALSE(unsubscribed.exchange(true))` so a second delivery **fails**; comment :1422-1423
rewritten. `ReuseAcrossWindows` is untouched (lawful; its read is free rung-4 proof). Every named
test was checked against tier 1: none asserts or tolerates a nested delivery, a delivery on the
publishing thread, or an uncounted row. **Red-first order:** write all forcing tests and guards,
record each red on `9ab5d86` (rung 5 under `-L heavy`), then implement §1–§5 in one change.

## Risks / Unknowns

- **Owner decision 1 (Brief).** `~SubscriberArrow` called from inside its own batch callback: L9's
  exception names only `Unsubscribe`, so L9 ("after `~SubscriberArrow` returns, no further batch"),
  L12 (no nesting) and L1 (no silent loss) cannot all hold — LL-1's collision, for the destructor.
  The mechanism treats it like an in-callback `Unsubscribe` (no special case); the owner decides
  whether L9's exception is amended to name it. No test until ruled (it would be a new subject).
- **Owner decision 2 (Brief).** d7 D10 and D-D7F-2 say the transport's thread "never waits on a
  callback". The owner's ruling text does not; it concerns row delivery. A user who `Unsubscribe`s a
  batched subscription *from* a transport thread (e.g. a per-row callback) waits for its last
  batches, as L9 requires. Suggested D10 wording: "batch assembly never makes the transport's
  thread run or wait on a callback". Today that same call runs `kClosing` on that thread.
- **Residue, pre-existing, unchanged:** callbacks of two subscriptions that each `Unsubscribe` the
  other wait on each other forever (today's timer joins do the same). Outside the ladder; recorded
  under BC-5, not designed around.
- **ABI:** no member added (§4 reuses `batchers_`); `RecordBatchBatcher` is cpp-private.
- **Coexistence window:** none. `Flush`'s delivery half, `TimerLoop` and the retry copy go in this
  stage; nothing later deletes what this stage adds.
- **Size:** within one stage, far under the 250 ceiling; no split proposed.

## Files-to-touch

- `pubsub-arrow/src/subscriber_arrow.cpp`: §1–§5.
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`: comments only (§4, §7).
- `pubsub-arrow/README.md`: §7.
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: rungs 3, 4, 6, three guards, ten re-anchors, the
  `BatchSink` comment (:481-482).
- `pubsub-arrow/tests/test_pubsub_arrow_heavy.cpp` (new) and `pubsub-arrow/tests/CMakeLists.txt`:
  `pubsub_arrow_heavy_tests`, `LABELS heavy`, a TIMEOUT stated with its measurement (file precedent).
- `pubsub-arrow/conanfile.py`: `cmake.test()` → `cmake.ctest(cli_args=["-LE", "heavy"])` (P5).
- `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp`: stale thread comments :263,
  :276-277.
- `docs/domains/d7-batch-assembly.md`, `docs/domains/bounded-context-register.md`,
  `plans/D7F-test-ladder.md`: §7.

**Declared net lines (production, cpp + header code): +90 / −60, net ≈ +30.** Tests ≈ +500 and docs
≈ +40 / −25, not counted. New public surface: **0**.

## Files-to-delete

- `Flush`'s delivery half (unlock, `cb_`, relock; cpp:219-228) → the batch loop's single `cb_` call.
- `TimerLoop` (cpp:167-183) → the batch loop, on the same thread.
- The `if (stopped_) return;` guard (cpp:87-89) → no replacement; behaviour gone (the silent drop).
- The duplicate append-and-classify retry block (cpp:90-99) → `TryAppend`, called twice.
- `Stop`'s inline `Flush(kClosing)` delivery (cpp:121) → `Close(kClosing)` queued behind the queue.
- `schema_set` and `schema_decided` in the batched lambda (cpp:324-327, 337-345) → `decided_`.
- Comments stating the retired behaviour: cpp:39-48 (rewritten), 185-188; test :545, :1223,
  :1355-1365, :1381-1382, :1397-1403, :1422-1423; `BatchSink` :481-482; fastdds :263, :276-277;
  d7 §4 :233-236. No test is deleted: audit 0a found no forbidden subject.
