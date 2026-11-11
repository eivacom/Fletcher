# D7F — test ladder

Round **D7F**, domain 7 (batch assembly), branch `feature/row-index`. Purpose (owner): every batch a
subscriber receives arrives one at a time and in order, and no lost row ever goes unreported, so that
rows a caller published in sequence reach the consumer in sequence (L14's precondition).

**Revision 2.** This revision applies the owner's answers to Q-A and Q2 (rul:139-155; d7 D10 and D11):
every batch callback runs on the subscription's own batch thread, full windows are queued for it in
order, and the ~256 MiB test is labelled `heavy`. Rungs 3-7 of revision 1 were re-derived for that
design; what changed is listed at the end.

Citations: `d7` = `docs/domains/d7-batch-assembly.md`, `rul` = `plans/RIX-rulings.md`,
`audit` = `plans/D7F-pre-round-audit.md`, `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`,
`test` = `pubsub-arrow/tests/test_pubsub_arrow.cpp`, `bd` = `arrow-bridge/src/batch_decoder.cpp`,
`ipp` = `pubsub/include/fletcher/pubsub/in_process_provider.hpp`.

Ceiling: **250 net production lines per stage** (tests not counted). Sizes are estimates of net
production lines. Doc text (header, README, d7) and test infrastructure (tests/CMake, conanfile) are
listed but not counted.

## Ground rules for every rung

- **Driven from outside:** `SubscriberArrow::Subscribe(segments, RecordBatchCallback, BatchOptions,
  TopicOptions)`, `Unsubscribe`, `~SubscriberArrow`, fed by `PublisherArrow::Publish/PublishDirect`
  (or `Publisher`). `RecordBatchBatcher` is never called by a test.
- **Providers.** The file-local `MockProvider` (test:31) delivers synchronously and hands a null
  `SharedSchema` for a topic never declared, then the real schema after `CreateTopic`.
  `InProcessPubSubProvider` (public, `ipp:63`) holds its instance mutex across each delivery and
  refuses re-entry at its door (`ipp:48-62`). In `as_declared` mode (the default), a subscription made
  before any declaration keeps a null schema for life (in_process_provider.cpp:236-239). Rungs 3-6 use
  `InProcessPubSubProvider`, because its mutex is the one domain 3's barrier waits on.
- **Overlap, order and thread are observed in the user callback**, which is external code. It keeps an
  atomic in-flight count and a per-thread nesting depth, and records `(reason, first row value,
  rows_dropped, thread id)` for each delivery. A callback that must stay "in flight" waits on a latch
  **with a bounded timeout**, so that a red run fails instead of hanging. The suite's ctest TIMEOUT of
  30 s (tests/CMakeLists.txt:33-34) turns any deadlock into a failure.

## The ladder

| # | Subject (what becomes TRUE) | External path | Internal observation | Builds on | New work (est. net prod. lines) | Authority |
|---|---|---|---|---|---|---|
| **1** | Rows that reach a batched subscription while it has **no usable schema** are reported when the subscription closes. `Unsubscribe` (and `~SubscriberArrow`) delivers exactly one `kClosing` batch, with `batch == nullptr` and `rows_dropped` = rows sent. The header and README state D9's wording. | `PublishDirect` on a topic never declared (the Mock passes a null schema), then batched `Subscribe`, then `Unsubscribe`. Same again with the destructor as the ending. Variant: InProcess (`as_declared`) subscribed before the declaration. | None. | The existing pipeline. `Flush` is the single window closer (census C4). | Drop the `ready_` gate on reporting (cpp:210). Build the null batch when there is no decoder. Fold `NoteDropped` (cpp:121-126) into one count entry (C2). Delete the dead branch cpp:69-71 (C4). **~30-50.** Docs: hpp:103-111 and README:94-98 take D9 verbatim. hpp:103-104 becomes "zero-row (or null) batch". | L1 (violation 1), L6, L7, L9; D9 (rul:118-132); "2. Fix both" (rul:83) |
| **2** | The same loss is reported **at the deadline, without waiting for a schema**. An open subscription whose rows have no usable schema receives a `kTimeout` null batch carrying that window's `rows_dropped`, once per window. A usable schema that arrives later gives normal batches. Σ`num_rows` + Σ`rows_dropped` = rows sent, across the transition. | Mock: `PublishDirect` before `CreateTopic`, then `WaitFor` the timeout delivery, then `CreateTopic`, good `Publish`es and `Unsubscribe`. Variant: InProcess `as_declared` subscribed early (one report per window). | None. | Rung 1's null-batch flush and single count entry. | Remove the timer's wait-for-`ready_` branch (cpp:194-199). Retire `ready_` as a reporting gate (C3). **~0-20, likely net negative.** | L1 (violation 1), L5, L6, L13; D9; "Fix both" |
| **3** | **Every `kRowLimit` and `kTimeout` callback runs on the subscription's batch thread, never on the publishing (transport) thread.** `Publish` returns without running or waiting on a callback. Batches arrive one at a time, in window order. | **InProcess**, `max_rows` = 2. The callback signals "entered", then waits (≤ 500 ms) for the publisher thread's "all Publishes returned". A second thread publishes 6 rows. Expect: every `Publish` returns while the first callback is still blocked. The callback thread id differs from the publisher's and is the same for every delivery. In-flight ≤ 1. First-row values strictly increase (0, 2, 4). Today the first callback runs inside `Publish`, so the wait times out: red. | None. | Rungs 1-2's finish logic. The existing timer thread becomes the batch thread. | Split `Flush` in two: finishing the window (under `mu_`) pushes `{batch, attachments, status}` onto an in-order queue, and delivery happens only in the batch thread's loop. `AddRow` never calls `cb_`. Lock order: `mu_` is never held across `cb_` or `Subscriber::Unsubscribe` (C5). **~40-80** (rungs 3-6 together). **Re-anchor the 10 tests** of audit A.5 here (`FlushesAtRowLimit` 528, `AttachmentsAlignWithRows` 596, `DictionaryColumnRefoldedToDictionaryArray` 683, `DictionaryColumnPreservesNulls` 723, `CorruptRowIsCountedDroppedAndBatchStaysAligned` 757, `FixedSizeListWithNamedItemArrivesNonNull` 827, `FinishFailureIsReportedNotFatal` 890 (block 911-916), `BatchesAreValidArrow` 1023, `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly` 943; `ReuseAcrossWindows` was listed in error, audit 0a) onto `sink.WaitFor` or a latch. Rewrite the comments at test:542 and test:932-942 and d7 §4's thread list. | D10 (rul:139-150); L12; L14's dependency (d7:187-189); "Fix both" |
| **4** | **Closing from outside goes through the same thread and the same order.** When `Unsubscribe` or `~SubscriberArrow` is called from another thread, every window already queued is delivered, then `kClosing` (on the batch thread), and only then does the call return. Nothing is delivered after it. This holds while a callback is in flight. | **InProcess.** A `kRowLimit` callback is held on a latch while 2 more full windows queue up and a partial window is pending. Then the main thread calls `Unsubscribe` and releases the latch from a helper thread. Expect deliveries `kRowLimit` ×3, then `kClosing`, all on the batch thread, all before `Unsubscribe` returns, and none after. Repeat with the destructor as the ending. | None. | Rung 3's queue and loop. | `Stop` from outside marks closing, wakes the loop and joins it. The loop finishes the pending window as `kClosing` **behind** the queue, drains, then exits. This replaces the inline `Flush(kClosing)` (cpp:136), which is pre-existing code. Counted in rung 3's estimate. | L9, L12, L1 (no queued window lost at close); audit row 57 |
| **5** | **A row that met the offset budget, during whose early flush the callback calls `Unsubscribe`, is counted.** The first delivery is a budget-forced `kRowLimit` holding only the earlier row. Σ`num_rows` + Σ`rows_dropped` = rows sent before `Unsubscribe`. **Test labelled `heavy`** (D11). | Mock or InProcess, large `max_rows`. `Publish` one good row on a `list<int8>` schema, then `PublishDirect` a crafted row: list count 2^31-1 and an all-null bitfield (~256 MiB). `SkipSink::ListBegin` throws `BatchCapacityExceeded` before any element is read (bd:159-162, 282-288), so the row is over the budget even alone. The `kRowLimit` callback calls `Unsubscribe`. Today Σ is short by 1: red. | None. A `kRowLimit` of 1 row under a large `max_rows` shows externally that the budget path ran. | Rung 3: the early flush now only enqueues, so the retry runs under `mu_` before any callback can call `Unsubscribe`. | Delete the uncounted `if (stopped_) return;` guard (cpp:93-95, made dead by rung 3) and the duplicate retry block cpp:96-104, keeping one classify-and-count path (C2). **~-10 to -20**, inside rung 3's estimate. | L1 (violation 2), L2, L4; D11 (rul:151-152) |
| **6** | **`Unsubscribe` from inside a batch callback never delivers a nested batch.** It returns. After that callback returns, any full windows already queued are delivered in order, then the `kClosing` batch carrying the window still pending, all on the batch thread, and nothing after it (L9 as clarified, Q-B ruling). | **InProcess.** (a) A `kTimeout` callback blocks while a second thread `Publish`es k < `max_rows` rows, then calls `Unsubscribe` and returns. Expect nesting depth ≤ 1, `Unsubscribe` returning inside the callback, then `kClosing` with k rows, then nothing. (b) The same from a `kRowLimit` callback. (c) Queued case: a `kRowLimit` callback is held while another full window queues, then calls `Unsubscribe`. Expect the queued `kRowLimit`, then `kClosing`, then nothing. (d) An `Unsubscribe` from a thread delivering rows on the same transport (a per-row callback) while a `kTimeout` callback is held returns at once; then `kClosing` on the batch thread (ruling 2026-11-11 (2)). (e) Destroying the `SubscriberArrow` inside a held `kTimeout` callback while a full window is queued: it returns at once, then the queued `kRowLimit`, then `kClosing`, then nothing (ruling 2026-11-11 (1)). Today `kClosing` is delivered nested: red. | None. | Rungs 3-4. In-callback `Stop` runs on the batch thread, which is the `t.get_id() == this_thread` case (cpp:143, L11). | In-callback `Stop` marks closing and returns without joining (it detaches, as today). The loop delivers `kClosing` once the callback returns. Counted in rung 3's estimate. Re-anchor test:999-1001: its comment, and the `exchange` guard, which becomes dead. | L9 as revised (rul:118-136) and widened (2026-11-11 (1), (2); ladder ruling 2026-11-11); L11; L12 |

**Rungs: 6. Stages: 3.** Stage 1 is rung 1, stage 2 is rung 2, and **stage 3 is rungs 3-6**, at about
40-80 net lines (well under the ceiling).

**Why stage 3 carries four rungs (flagged; this is not a ceiling exception).** Under D10, L12, closing
order, L1's violation 2 and the revised L9 all follow from **one** mechanism: a queue that only the
batch thread drains. Each way of splitting it breaks a rule:
- Rung 3 alone would lose queued windows at close, which would be a new L1 violation.
- Rung 4 or rung 6 built first would need an interim closing path that stage 3 then deletes.
- Rungs 5 and 6, after rungs 3-4, arrive green with no work of their own, so they cannot be stages.

The four subjects stay separate rungs, so each one is proven separately. The architect must order
stage 3's red-first work so that each rung's test is shown red on the pre-stage tree.

**No earlier stage builds a construct that stage 3 deletes.** Stage 1 changes only the window-finishing
half of `Flush` (null batch, the count entry), which stage 3 keeps as is when it splits `Flush`. Stage
2 only removes code. The architect of stage 1 should place the null-batch construction in the part of
`Flush` that runs under `mu_`, so that stage 3 moves the delivery half and leaves it alone.

## Tests other than rungs (same promises, every path)

- **Guard (replaces revision 1's rung 6).** D10 makes the old deadlock unrepresentable: the
  transport thread never waits on the batch side. A declared guard in stage 3 witnesses it. A callback
  calls `Unsubscribe` while another thread is inside `Publish` (holding the in-process mutex). Both calls
  return within the ctest TIMEOUT. The guard is green in both directions only if stage 3 is correct,
  and it must be declared a guard.
- **Conservation property (C2, BC-2), stage 3.** Random mixes of good, malformed and null-schema rows,
  ended by `Unsubscribe` from outside or from inside the callback, satisfy Σ`num_rows` +
  Σ`rows_dropped` = rows offered before `Unsubscribe`.
- **States × endings (C3, BC-3), stage 2.** Each of awaiting-schema, undecodable and assembling, ended
  by a timeout or by closing, reports its drops.
- **Order stress (C1, BC-1), stage 3.** `max_rows` 1..3, `timeout` 1 ms, `Unsubscribe` at random
  from inside and outside the callback. Expect: in-flight ≤ 1, every callback on one thread, first-row
  values strictly increasing, and `kClosing` last.
- **Controls.** Each rung is deterministically red on the current tree:
  - 1: no delivery.
  - 2: `WaitFor` times out.
  - 3: the latch wait times out inside `Publish`.
  - 4: `kClosing` overlaps the in-flight callback, or runs on the `Unsubscribe` thread.
  - 5: Σ is short by 1.
  - 6: nesting depth 2.

## The `heavy` label (rung 5, D11)

**Today:** no `CMakeLists.txt` in the repo sets `LABELS`. `pubsub-arrow/tests/CMakeLists.txt:33-34`
registers every gtest row with one `gtest_discover_tests(pubsub_arrow_tests DISCOVERY_MODE PRE_TEST
PROPERTIES TIMEOUT 30)`, and `pubsub-arrow/conanfile.py:61-62` runs `cmake.test()`, which runs every
registered test with no label filter. Adding a label alone therefore would **not** keep the test out
of the default run. Stage 3 needs three test-infrastructure changes, none of them counted as
production lines:
1. **Its own executable.** Put the heavy test in `tests/test_pubsub_arrow_heavy.cpp`, built as
   `pubsub_arrow_heavy_tests`. It uses `InProcessPubSubProvider`, so it does not need the file-local
   Mock. Register it with `gtest_discover_tests(pubsub_arrow_heavy_tests DISCOVERY_MODE PRE_TEST
   PROPERTIES LABELS heavy TIMEOUT <measured × margin, stated with the measurement as the file's
   precedent requires>)`. A separate executable also keeps a direct run of `pubsub_arrow_tests.exe`
   light.
2. **Exclude it from the default run.** In `conanfile.py`, `cmake.test()` becomes
   `cmake.ctest(cli_args=["-LE", "heavy"])`. Conan 2's `CMake.ctest` runs under the run environment,
   which matters on Windows (see the note at xrcedds-pubsub-provider/conanfile.py:59-63). Confirm the
   installed Conan provides it.
3. **Run it on request:**
   ```
   conan build pubsub-arrow --build=missing -pr:a=.conan-profiles/Windows-msvc194-x86_64-Release -o "&:run_tests=True"
   pubsub-arrow\build\generators\conanrun.bat
   ctest --test-dir pubsub-arrow\build -C Release -L heavy --output-on-failure
   ```
   The first command builds and runs the default (light) set. The last runs only `heavy`. Verify the
   build-folder path that `cmake_layout` produces at stage 3. The inner loop (`conan create ...`) stays
   light.

## Status of earlier questions

- **Q-A:** answered ("Label it 'heavy'", D11). Applied above.
- **Q-B: answered 2026-11-10** ("Queued batch, then closing", [RIX-rulings.md](RIX-rulings.md)).
  Windows already queued are delivered first, in order, then `kClosing`, then nothing. L9 is clarified
  to match. Rung 6 now asserts the queued case (c).
- **The C6 disposition probably changes.** Under D10, "is this thread inside this batcher's callback"
  is the same question as "is this the batch thread", and cpp:143 already answers it. The audit's
  REUSE of `DeliveryScope` would add nothing. This is the architect's call, and BC-6's register entry
  should follow it.

## Changes from revision 1

| Rev 1 | Rev 2 | Why |
|---|---|---|
| Rung 3: same-thread deferred `kClosing` via `DeliveryScope` | Rung 6, after the move | The move deletes the old construct (the post-callback drain in `Flush`). Built first, a later stage would delete it. |
| Rung 4: V2, its own stage | Rung 5, inside stage 3 | Once `Flush` only enqueues, the guard is dead and the row is retried under `mu_`. The rung is green with the move. |
| Rung 5: serialize cross-thread callbacks | Rung 3: everything on the batch thread | D10 chose a single flushing thread. The subject gains "`Publish` never runs or waits on a callback". |
| Rung 6: no deadlock with a waiting delivery thread | **Dropped as a rung**, kept as a declared guard | Its subject no longer exists: the delivery thread never waits (D10). |
| Rung 7: outside `Unsubscribe` waits out an in-flight callback | Rung 4 | The subject is unchanged. `kClosing` now runs on the batch thread, behind the queue. |
