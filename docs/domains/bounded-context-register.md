# Bounded-context register

A bounded context is a question the running system answers where two places answering differently
would be a defect: a rule, not a mechanism. Each row names the **single site** allowed to answer
it, a **structural test** that fails on a violation nobody listed, and a build-state mark scoped to
what is actually built.

Created 2026-11-10 by the D7F pre-round census ([plans/D7F-pre-round-audit.md](../../plans/D7F-pre-round-audit.md),
part B). BC-1, BC-5 and BC-6 are adapted to the later owner ruling that every batch callback runs on
the batch thread (domain 7 D10). A new row is added whenever a round's census finds a question with
no row. A change to a row's single site needs the round that changes it to say so.

| ID | Question | Single site | Structural test | Status | Domain |
|---|---|---|---|---|---|
| BC-1 | May a batch be handed to the callback now, and is it the next in window order? | `RecordBatchBatcher`'s batch-thread loop: the only call to `cb_`. Finished windows go onto one in-order queue under `mu_`. | Order stress: `max_rows` 1–3, `timeout` 1 ms, random `Unsubscribe` from outside and inside the callback. Assert in-flight ≤ 1, every delivery on one thread, and strictly increasing first-row values. | `[designed-not-built]`, D7F-3 | d7 (L12) |
| BC-2 | Is this row delivered, dropped or neither? | `RecordBatchBatcher`'s one classify-and-count function (`NoteDropped` folded in; the retry copy at `cpp:96-104` deleted). | Conservation property: random good, malformed and schema-less rows, ended by `Unsubscribe` from outside or inside the callback. Σ`num_rows` + Σ`rows_dropped` = rows offered before `Unsubscribe`. | `[designed-not-built]`, D7F-1 and D7F-3. **Registered gap:** the per-row `SubscriberArrow::Subscribe` drops silently when it has no codec (`cpp:309-311`); out of D7F scope. | d7 (L1, L2) |
| BC-3 | Which assembly state is this subscription in, and is its schema usable? | `RecordBatchBatcher`'s one state (awaiting schema / assembling / undecodable / closed), fed by one usability decision. `AcquireCodec`'s import failure becomes a state, not a `return nullptr`. | Every state × {timeout, closing}: a schema-less row then the ending delivers exactly one batch with `rows_dropped` ≥ 1. | `[designed-not-built]`, D7F-1 and D7F-2 | d7 (L1, L6, L8) |
| BC-4 | When does a window close, and why? | The window-finishing half of `RecordBatchBatcher::Flush` (dead branch `cpp:69-71` deleted). | The existing `FlushesAt*` tests plus BC-2's property. | `[built]`. **Registered:** `kRowLimit` covers two causes, row count and offset budget (d7 Q4). | d7 (L4, L5) |
| BC-5 | After `Unsubscribe` or destruction returns, can anything still be delivered? Who waits for whom? | Row side: `Subscriber`'s `RetireAndDrain` (domain 3). Batch side: `RecordBatchBatcher::Stop`. Lock order: batcher `mu_` is never held across `cb_` or across `Subscriber::Unsubscribe`; the transport's delivery thread never waits on the batch thread (D10). | `Unsubscribe` from inside a `kTimeout` callback while another thread is filling windows, under the ctest TIMEOUT: a deadlock fails it. Plus ladder rung 4's ordering assertions. | Batch side `[designed-not-built]`, D7F-3; row side `[built]` | d3 + d7 (L9, L11) |
| BC-6 | Is this thread inside a delivery on object X? | `core/include/fletcher/core/internal/delivery_frame.hpp` (`InsideDeliveryOn` / `DeliveryScope`). No other `thread_local` may answer it. Under D10, "inside this batcher's callback" equals "on this batcher's thread" (`cpp:143`); whether the batcher also pushes a `DeliveryScope` token is the D7F-3 architect's call. | The existing caller-tier door tests. | `[built]` for domain 3 | d3 (shared) |
| BC-7 | Is a contained callback failure counted? | Two answers today: `Subscriber` counts (`AbsorbedCallbackFailures`); the batcher swallows without counting (`cpp:243-246`). | None yet. | **Deferred.** Two answers; an owner ruling is owed (a new law or an accepted difference). | d3 / d7 |
