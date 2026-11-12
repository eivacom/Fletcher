# D7F — pre-round audit (Gate 4 consistency, Gate 4b bounded-context census)

Round **D7F**, domain 7 (batch assembly), branch `feature/row-index`, working tree after `ebcf4a7`.
Purpose: fix L1 (violations 1 and 2) and L12 before RIX (ruling "2. Fix both", `plans/RIX-rulings.md:83`).
Citations: `d7` = `docs/domains/d7-batch-assembly.md`, `hpp` = `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`,
`cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`, `README` = `pubsub-arrow/README.md`, `test` = `pubsub-arrow/tests/test_pubsub_arrow.cpp`,
`sub` = `pubsub/src/subscriber.cpp`, `frame` = `core/include/fletcher/core/internal/delivery_frame.hpp`, `rul` = `plans/RIX-rulings.md`.
Tier-2 corrections below are **suggestions only**; none is applied.

## A. Consistency audit

### A.1 Operational clauses

| # | Clause (quoted, file:line) | Law | Contradicts? | Verdict | Suggested correction |
|---|---|---|---|---|---|
| 1 | "A lost row is counted and never recovered" d7:28-29 | L1 | no | OK | — |
| 2 | "Out-of-range values are clamped silently, never refused" d7:30-31 | NONE | no | FINDING | No law governs; owner Q5 (d7:251). Not D7F scope |
| 3 | "`BatchOptions{max_rows = 8000, timeout = 1 min}`" d7:190 | L4 | no | OK | — |
| 4 | "`RecordBatchBatcher` is private ... Only `SubscriberArrow` constructs or drives it" d7:197 | NONE | no | FINDING | Structural rule with no law; make it register entry BC-1's single-site premise |
| 5 | "`TopicOptions` passes through to domain 3 untouched" d7:198 | NONE (§1 Refuses) | no | FINDING | Delegation rule; no action beyond noting it is §1's, not a law |
| 6 | "call `Subscribe` and `Unsubscribe` from any thread, including from inside the batch callback" d7:201-202 | L9 | **yes — with L12 and L1 (LL-1)** | **BLOCKER** | Owner ruling needed; see LL-1 |
| 7 | "callback runs on the provider's delivery thread for `kRowLimit` ..., timer thread for `kTimeout` ..., thread calling `Unsubscribe` ... for `kClosing`" d7:203-206 | L12 | no (lists threads, does not license overlap) | FINDING | Rewrite after Q2 is designed: name the one serialization, and the thread if a single flushing thread is chosen |
| 8 | "The callback target must outlive the subscription" d7:207 | L11 | no | OK | — |
| 9 | D1 "A window with only drops still delivers ... A partial window is flushed as `kClosing`" d7:213 | L1,L3,L6,L9 | no | OK | "decision A" still unsourced (Q6) |
| 10 | D2 "timeout is anchored to the previous flush, with a 1 ms floor" d7:214 | L5 | no | OK | — |
| 11 | D5 "skips provider teardown when a handler cancels its own last subscription" d7:217 | L9 | no | OK | — |
| 12 | D6 "L1's two violations and L12's violation are fixed in a round before RIX" d7:218 | L1,L12 | no | OK | — |
| 13 | "delivery is serialized per subscription ..., which makes `AddRow` single-producer" d7:228-230 | L2,L12 | no | OK | Note: the timer thread is a second *flusher* (cpp:195), which this does not cover |
| 14 | "`Subscriber::Unsubscribe` acts as a barrier ... together with `Stop` gives L9" d7:230-231 | L9 | no, but see hazard | FINDING | Add: a batch-delivery serialization held while `sub:247` waits on the row gate deadlocks (kTimeout callback → Unsubscribe → barrier waits for a delivery thread blocked on the batch serialization). Register as lock-order entry BC-5 |
| 15 | "rows can arrive before `Subscribe` returns" d7:232 | L13 | no | OK | — |
| 16 | Q1 "for example as a null batch at closing or at the deadline" d7:247 | L1,L6,L7 | yes — every candidate breaks hpp:106-111 (row 31) | FINDING | Folded into BLOCKER B2 |
| 17 | Q2 "A delivery lock versus a single flushing thread" d7:248 | L12 | partly | FINDING | A lock taken *after* `mu_` is released (cpp:238) serializes but does not order: the delivery thread can win the lock for window N+1 before the timer thread takes it for window N. Add "...that also preserves window order (ticket/sequence taken under `mu_`)" |
| 18 | §8.1 "No row may leave the window uncounted" d7:257-259 | L1,L2 | no | OK | Applies directly to V2's fix |
| 19 | §8.2 side data appended/cleared/moved with the batch d7:260-262 | L3 | no | OK | — |
| 20 | §8.3 "Decide windows here, decode in domain 2" d7:263-264 | L4 | no | OK | — |
| 21 | §8.4 "User code and allocation failures stay in `try` blocks" d7:265-266 | L10,L11 | no | OK | A new serialization primitive's `lock()` can throw `system_error`; it falls under this clause |
| 22 | §8.5 "Write any change to a law here before writing the code" d7:267-268 | d7:3-4 | no | OK | — |
| 23 | "Stops and flushes any batched subscriptions ... before teardown" hpp:37-38 | L9 | no | OK | — |
| 24 | "`kRowLimit, // flushed because max_rows was reached`" hpp:81 | L4 | yes (L4: also the offset budget) | FINDING | Append "or the 32-bit offset budget was hit (see Subscribe)"; Q4 decides a new enum |
| 25 | "`rows_dropped; // rows lost since the previous flush`" hpp:86 | L1,L2 | no (code breaks it, V1) | OK | — |
| 26 | "a window that would overflow a 32-bit Arrow offset ... is flushed early with reason kRowLimit" hpp:91-93 | L4 | no | OK | — |
| 27 | "`options.timeout` elapses since the first row/drop of the current batch" hpp:95-97 | L5 | yes | FINDING | "...elapses since the previous flush (at least 1 ms after the window's first row/drop)" (d7 Findings a1) |
| 28 | "A partial batch is flushed on Unsubscribe with reason kClosing" hpp:97-98 | L9 | no (code breaks it when `!ready_`, cpp:210) | OK | — |
| 29 | "`attachments[i]` belongs to batch row `i`" hpp:100 | L3 | no | OK | — |
| 30 | "If a window contains only dropped rows, a zero-row batch is still delivered" hpp:103-104 | L6 | yes (L6: null when no decoder; V1's fix adds a no-schema case) | FINDING | "...a zero-row batch (or a null batch, see below) is still delivered" |
| 31 | "`batch` is null only when `BatchDecoder`'s constructor rejected ... for the entire life of this subscription" hpp:106-111 | L7,L8 | **yes — L7 (Finish failure, cpp:217-225); and any V1 fix that reports pre-schema drops as a null batch followed by normal batches** | **BLOCKER** (B2) | Owner to approve, with Q1 and Q3: "`batch` is null when no batch could be built for the window: no usable schema has arrived yet, the schema was rejected (permanent), or Finish failed. `rows_dropped` counts the window's rows either way." |
| 32 | dictionary-shape fallback rule hpp:113-122 | NONE (domain 2) | no | FINDING | Out of D7F scope; belongs to d2 (d7 Findings b4) |
| 33 | "Starts the timer thread ... co-owner ... Must be called once, right after construction" cpp:39-48 | L11 | no | OK | — |
| 34 | "Flush() runs the user callback with mu_ released" cpp:40-41; "the callback runs with it released" cpp:205-206 | L12 | **yes** — the construct that lets callbacks overlap | FINDING | D7F retires it; the comments must name the replacement serialization |
| 35 | "Until a non-null schema is set the batcher buffers but cannot build batches" cpp:53-54 | L1 | yes — nothing is buffered; drops are counted then discarded (cpp:121-126, 210) | FINDING | Rewrite with the fix |
| 36 | "every row is then counted dropped and the periodic report carries a null batch" cpp:55-57 | L1,L6,L8 | no | OK | — |
| 37 | "nothing may escape: every failure is folded into the drop count" cpp:75-77 | L10,L1 | no | OK | — |
| 38 | "close this batch and start the next one with it" cpp:90-91 | L4 | no | OK | — |
| 39 | "it may have called Unsubscribe -> Stop() ...; don't touch the decoder if so" + `if (stopped_) return;` cpp:93-95 | L1 | **yes** (V2: row neither appended nor counted) | FINDING | Fix target; see LL-1 for why the fix needs a ruling |
| 40 | per-branch drop comments cpp:100-111 | L1,L2 | no | OK | — |
| 41 | "A row that failed to decode: counted as lost" cpp:119-120 | L1 | no (code loses it, V1) | OK | — |
| 42 | "delivers any pending rows/drops (reason kClosing). Idempotent." cpp:128-129 | L9,L1 | no | OK | — |
| 43 | "Don't join ourselves if a callback on the timer thread called Stop()" cpp:141 | L11 | no | OK | — |
| 44 | DiscardPending "count it dropped whole ... nothing may escape" cpp:154-158 | L1,L10 | no | OK | — |
| 45 | "The deadline is anchored to the previous flush" cpp:168-172 | L5 | no | OK | — |
| 46 | "Deadline reached but schema not ready yet — wait for it." cpp:197 | L1 | **yes** (V1: waits forever) | FINDING | Fix target |
| 47 | "a window with only dropped rows still delivers a zero-row (or null ...) batch" cpp:203-205 | L6 | no | OK | — |
| 48 | `if (!ready_) return; // schema not set — cannot build` cpp:210 | L1,L6 | **yes** (V1, incl. at `kClosing`) | FINDING | Fix target |
| 49 | "Finish() ... stays under the lock ... never sees a half-built batch" cpp:213-215 | L3 | no | OK | — |
| 50 | "the window is dropped whole" cpp:221-223 | L1,L7 | no | OK | — |
| 51 | "Isolate the user callback ... must not depend on user code being noexcept" cpp:239-242 | L10 | no | OK | Uncounted, unlike domain 3 (census C7) |
| 52 | "Tear down the Subscriber here, while mu_/codecs_ are still alive" cpp:284-287 | L9,L11 | no | OK | — |
| 53 | "Codecs live in `codecs_` for the SubscriberArrow's lifetime" cpp:345-347 | L1 | yes — `Unsubscribe` erases them (cpp:448-450); race → `SetSchema(nullptr)` (d7 Findings a3) | FINDING | Either keep codecs for the lifetime or correct the comment; with V1's fix the race must at least be reported |
| 54 | "If no codec can be built, count the message as dropped so the loss is reported" cpp:358-359 | L1 | no (code loses it, V1) | OK | — |
| 55 | "Hand the batcher the schema on the first decodable sample ... (idempotent)" cpp:369-371 | L13 | no | OK | — |
| 56 | "non-blocking ... without dropping the first window for want of a schema" cpp:391-394 | L13 | no | OK | — |
| 57 | "Flush the partial batch (reason kClosing) and join the timer before the Subscriber stops delivering" cpp:426-427 | L9 | yes w.r.t. L12: the kClosing callback can run while a kRowLimit callback is still in flight on the delivery thread (that one is waited for only later, at cpp:429) | FINDING | Stop must wait out any in-flight batch callback before its kClosing delivery (or hand it to the serializer) |
| 58 | "free the codec entry if this was the last subscription" cpp:431-432 | L1 | yes, with row 53 | FINDING | Same as 53 |
| 59 | "a timeout elapses since the batch started filling" README:66-68 | L5 | yes | FINDING | As row 27 |
| 60 | "a window with only dropped rows still delivers a zero-row batch ... flushed with reason `kClosing` on `Unsubscribe`" README:85-90 | L6,L9 | partly (null case) | FINDING | As row 30 |
| 61 | "delivers a null `batch` ... for the entire life of the subscription — there is no later recovery" README:94-98 | L7,L8 | **yes** (as row 31) | **BLOCKER** (B2) | Same amendment as row 31 |
| 62 | "flushed early with reason `kRowLimit`, same as hitting `max_rows`" README:112-114 | L4 | no | OK | — |
| 63 | "nested value types ... are rejected with a clear error" README:146-147 | L8 | yes (null batch + drops, no error) | FINDING | "...the batched overload counts every row dropped (null batch); see above" |
| 64 | "nested value types ... and `float16` are rejected with a clear error" docs/wire-format-specification.md:218-219 | L8 | yes | FINDING | As row 63 |
| 65 | "rejected by that batched path at subscribe time" wire-format-specification.md:224-226 | L8,L13 | yes (first decodable row, cpp:372) | FINDING | "...at the first decodable row, as permanent drops" |
| 66 | "The batched `PubSubArrow::Subscribe` ... rejected with a clear error" docs/architecture-overview.md:188-193 | L8 | yes | FINDING | Rename to `SubscriberArrow`; as row 63 |
| 67 | "The count trigger flushes synchronously on the publishing thread." test:542 | L12 (via d7:203) | no — rides a construct | FINDING | Conditional RE-ANCHOR (A.5) |
| 68 | "Stop()'s own kClosing flush ... re-enters this callback once more" test:999-1000 | L12 | **yes** — states a nested (overlapping) callback as expected | FINDING | Rewrite after LL-1's ruling |

### A.2 Laws vs laws
- **LL-1 — BLOCKER: L9 × L12 × L1 when `Unsubscribe` is called from inside a batch callback and the window is non-empty.**
  L9 needs the pending window delivered as `kClosing` *before* `Unsubscribe` returns (d7:135-137). L12 forbids a second
  callback while the first is still running (d7:156). L1 forbids discarding it (d7:58-59). The calling callback has not
  returned, so a delivery before `Unsubscribe` returns is nested, and anything later is after it returns. All three can hold
  only if the window is empty. It is non-empty in two reachable cases: (a) a `kTimeout` callback on the timer thread, while
  the delivery thread keeps appending under `mu_` (cpp:238, cpp:78-117); (b) **V2's own fix**: once the over-budget row is
  counted (cpp:95), it is pending when the `kRowLimit` callback's `Unsubscribe` runs `Stop`. So "Fix both" cannot be met as
  written. Rulings that would resolve it: (i) "one at a time" means *never concurrent across threads*. A `kClosing` that a
  callback requests through its own `Unsubscribe` is delivered nested, on that thread, before `Unsubscribe` returns. This is
  what the code does today (test:999-1000). (ii) L9's "after `Unsubscribe` returns" is measured from the return of the
  outermost callback. (iii) the remaining window is reported in some other way (an interface change).
- L1 × L9: consistent. L1 covers rows "before closing", and L9 leaves rows after closing uncounted (d7:58, 137).
- L6 × L7 × L8: consistent. However, once V1 is fixed with a null batch, a pre-schema null batch and a rejected-schema null
  batch look the same, so the caller cannot see L8's permanence. Q1's design must say whether that matters. FINDING.
- L11 × L12: consistent. Under serialization, `Stop` from another thread must wait out the timer's in-flight callback
  before its `kClosing` (row 57). `Stop` on the timer thread still detaches (cpp:143-144).
- L2 × L1: consistent once fixed. L2's only exceptions are L1's (d7:82).
- L12 × L4/L5/L10/L13 and L14: no conflict. L14 depends on L12 (d7:182-184).

### A.3 Laws vs rulings
- "2. Fix both" (rul:83) ↔ L1 and L12: they agree in subject, but LL-1 makes the pair unsatisfiable with L9 for V2.
  Escalate with LL-1.
- "Yes on all accounts" (rul:107) ↔ L1-L14: it confirms L9 and L12 together, so LL-1 lies between two confirmed laws.
- L14 ruling (rul:75) ↔ L12, L14: they agree. "no counting check" (rul:26) ↔ L14: they agree. D7F must not add an index
  check.
- Domain 3's 2026-09-05 re-entry rulings (d7:217, frame:131-137) ↔ L9 in-callback `Unsubscribe`: they agree. The provider
  door is skipped at sub:735, never hit.
- Data-model, Option-A, writer-hash, PR #104 and ownership rulings (rul:6-35, 59-104) ↔ L1-L13: no shared subject.

### A.4 Rulings vs rulings
No explicit contradiction. There is one implicit one, through LL-1: rul:83 ("Fix both") × rul:107 (L9 and L12 confirmed as
written). Confirming both laws and fixing V2 cannot all be done without a reading of L12 or L9 that the ledger does not
yet record.

### A.5 Test corpus audit
No test asserts an unreported drop, and no test depends on concurrent or out-of-order callbacks. **DELETE: none.**
- Not batch assembly, no verdict: `PublisherArrowTest.*` ×7 (test:161-266), `SubscriberArrowTest.*` ×5 (test:290-361),
  `PubSubArrowTest.*` ×4 (test:374-447), `NodiscardTest.CompileFailsOnDiscard` (compile-only, discard_probe.cpp:78-88,
  tests/CMakeLists.txt:82), and integration tests `SchemaAndRowDeliveredAcrossDdsBoundary`, `MultipleRowsDeliveredInOrder`
  and `SubscribeBeforePublishDeliversWithSchema` (per-row; integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp:92,150,318).
- No disabled or skipped tests exist (no `DISABLED_` or `GTEST_SKIP` under `pubsub-arrow/` or the integration dir).
- Fixture `BatchSink` (test:478-505) locks around each delivery. It tolerates concurrency but does not depend on it: KEEP.
- KEEP, lawful and independent of the construct: `SubscribeForwardsTopicOptions` (513), `FlushesAtTimeout` (551),
  `ClosingFlushOnUnsubscribe` (574), `DroppedRowReportedAndAttachmentDiscarded` (621),
  `OnlyDroppedRowsStillDeliversEmptyBatch` (655), `NestedDictionarySchemaReportsEveryRowDropped` (861; pins L8's null, which
  is lawful), and the integration test `BatchedRecordBatchDeliveredAcrossDdsBoundary` (test_roundtrip.cpp:214; waits on a
  cv, 271-274).
- **RE-ANCHOR, conditional on Q2 choosing a single flushing thread.** These read `deliveries` right after `Publish` with no
  wait. That rides "kRowLimit is delivered synchronously on the delivery thread" (test:542, d7:203), not a law:
  `FlushesAtRowLimit` (528), `AttachmentsAlignWithRows` (596), `DictionaryColumnRefoldedToDictionaryArray` (683),
  `DictionaryColumnPreservesNulls` (723), `CorruptRowIsCountedDroppedAndBatchStaysAligned` (757),
  `FixedSizeListWithNamedItemArrivesNonNull` (827), `FinishFailureIsReportedNotFatal` (890, block 911-916),
  `BatchesAreValidArrow` (1023), `ReuseAcrossWindows` (1062; its order check 1084-1091 is a positive L12 witness, so keep
  it), `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly` (943; its subject, re-entrant Unsubscribe on the
  delivery thread, would move threads). Re-anchor each on `sink.WaitFor`. If a lock design is chosen, all of them stay as
  they are.
- **RE-ANCHOR the comment of `UnsubscribeFromInsideATimeoutFlushIsSafe` (981).** Its test:999-1000 states a nested `kClosing`
  re-entry. The assertions (1018) are lawful. Rewrite the comment to match LL-1's ruling. Under ruling (ii) or (iii) the
  `exchange` guard at 1001 becomes dead code.
- `FinishFailureIsReportedNotFatal` contradicts hpp:106-111 (d7:126-127). The law (L7) sides with the test, so it is not a
  forbidden assertion.

**Part A totals: 68 clauses audited. 38 OK, 27 FINDING, 3 BLOCKER** (rows 6, 31 and 61). Plus law-level BLOCKER **LL-1**.
B2 (rows 31 and 61): the owner must approve the header and README amendment that V1's reporting channel (Q1) requires.

## B. Bounded-context census

| # | Question (a rule) | Operations searched, and where | Incumbent(s) found | Disposition |
|---|---|---|---|---|
| C1 | **May a batch be handed to the callback now, and is it the next one in window order?** | Mutex held across a user callback; every `cb_(` call site; ticket, sequence or condition-variable handoff, in `pubsub-arrow/src`, `sub`, `frame`, `pubsub/src/delivery_channel.cpp` | Batcher: none. `cb_` is invoked only at cpp:244, with `mu_` released (cpp:238). Domain 3: the per-subscription `Gate::mu` held across the row callback (sub:58-61, sub:415), contract provider.hpp:178-180 | **DEFINE** in `RecordBatchBatcher`: one delivery site (the sole `cb_` call) plus a window sequence taken under `mu_` at finish time. Do not reuse `Gate`: it is private to `Subscriber::Impl`, only delivery threads take it, and the barrier waits on it (sub:247). The timer thread taking it would make the deadlock in row 14 representable. **Structural test:** a stress test with `max_rows=1..3`, `timeout=1ms` and random `Unsubscribe` from outside and from inside the callback. The callback keeps an atomic in-flight count and records the first row value of each batch, then asserts in-flight ≤ 1 (≤ 1 per thread nesting, per LL-1's ruling) and strictly increasing first-row values. This fails on any overlapping path, listed or not |
| C2 | **Is this row counted as delivered, dropped, or neither?** | `++dropped_`, `dropped_ +=`, `atts_.push_back`, early `return` before counting, and `catch` bodies, in cpp; absorbed counters in `sub` and delivery_channel | Six count sites (cpp:82, 100, 102, 109, 124, 160). Two entry paths (`NoteDropped` cpp:121-126 vs the `AddRow` no-decoder branch cpp:81-85; d7 Findings b1). A duplicated retry classifier (cpp:96-104 vs 86-112; b3). An uncounted return (cpp:95). The per-row overload drops silently (cpp:309-311) | **EXTEND-AND-NARROW**: one classify-and-count function in the batcher. Delete `NoteDropped` as a separate path (fold it into the one entry) and delete the retry copy cpp:96-104. The per-row silent drop cpp:309-311 is **DEFER-BUT-REGISTER** (a domain-boundary API with no drop channel). **Structural test:** a conservation property. Random mixes of good, malformed and schema-less (`SharedSchema{}`) samples, ended by `Unsubscribe` from outside or inside the callback, must satisfy Σ`num_rows` + Σ`rows_dropped` = rows offered before `Unsubscribe` was called (the mock delivers synchronously). It fails on any uncounted path |
| C3 | **Is this subscription's assembly awaiting a schema, assembling, undecodable, or closed? Is a schema usable?** | Flags and nullness checked before acting: `ready_`, `decoder_`, `stopped_`, `once_flag`, `cached_codec`, `codecs_.find`, `batchers_` membership, `retired` | `ready_` (cpp:68, 114, 194, 198, 210), `decoder_` null (cpp:81, 152, 217), `stopped_` (cpp:80, 95, 123, 134, 184), `schema_set` (cpp:344, 372), `cached_codec` (cpp:348, 360-363), `codecs_` (cpp:376-377, 400-401). Usability is decided twice: `ImportArrowSchema` (cpp:403-409, unreported) vs the `BatchDecoder` ctor (cpp:62, reported; b2). Domain 3's `Gate::retired` (sub:60, 418) answers a different question (may the *row* callback run) | **EXTEND-AND-NARROW**: one batcher state (awaiting-schema / assembling / undecodable / closed) as the single answer. Narrow away `ready_`-as-a-gate-on-reporting (cpp:198, 210: this is the V1 fix), and route `AcquireCodec`'s import failure into the batcher as a state instead of `return nullptr`. **REUSE** `Gate::retired` unchanged. **Structural test:** a schema-less sample, then `Unsubscribe`, delivers exactly one batch with `rows_dropped` ≥ 1. It is parameterized over every state × {timeout, closing}, and fails if any state suppresses reporting |
| C4 | **When does a window close, and for what reason?** | Every `Flush(` call, deadline arithmetic, `has_deadline_` writes | `Flush` at cpp:70 (dead; b5), 92, 115, 136, 195. Deadline at cpp:176. Clear at cpp:208 | **REUSE** `Flush` as the single closer. **Narrow:** delete the dead branch cpp:69-71. The two-cause `kRowLimit` (Q4) is **DEFER-BUT-REGISTER** |
| C5 | **After `Unsubscribe` or destruction returns, can anything still be delivered, and who waits for whom (lock order)?** | `join`/`detach`, barrier waits, `DeferUntilDeliveryDepthZero`, teardown order | Batcher `Stop` join/detach (cpp:141-148), order Stop→`Subscriber::Unsubscribe` (cpp:428-429), `~SubscriberArrow` (cpp:275-288). Domain 3: `RetireAndDrain` (sub:216-247), the depth-0 sweep (frame:104-122), lock order "gate < mu" (sub:172-196) | **REUSE** domain 3's barrier for the row side. **EXTEND** `Stop` as the single batch-side site: it must also wait out an in-flight batch callback before `kClosing` (row 57). **DEFINE** the lock order: *batcher `mu_` < batch-delivery serialization; neither held across `Subscriber::Unsubscribe`*. **Structural test:** an `Unsubscribe` from inside a `kTimeout` callback while a second thread is mid-`kRowLimit`, run under the ctest TIMEOUT. A deadlock fails it |
| C6 | **Is this thread inside a batch callback of this batcher?** (needed to choose between a nested `kClosing` and a deferred one, per LL-1, and to avoid self-wait) | `std::this_thread::get_id()` compares, `thread_local` stacks, `InsideDeliveryOn` | `t.get_id() == this_thread::get_id()` (cpp:143) answers "am I the timer thread", a different question. `frame:51, 63, 104-126` (`DeliveryScope`/`InsideDeliveryOn`, "shared by every component that dispatches a subscriber callback", frame:4-6), used at sub:217, 735 | **REUSE** `internal::DeliveryScope` with the batcher's address as the token. Tokens of distinct objects never collide (frame:12-19). Keep cpp:143 for the join decision only. Pushing a token on the timer thread postpones domain 3's depth-0 sweep until the batch callback returns, which is the safe direction (frame:75-88). The register entry must state it. Do **not** add a new `thread_local` |
| C7 | **Is a contained callback failure counted?** | `catch (...)` around user callbacks, `fetch_add` on absorbed counters | Batcher: swallowed, not counted (cpp:243-246). Domain 3: counted, `absorbed_here` (sub:427-441), `AbsorbedCallbackFailures()` (subscriber.hpp:269), `DeliveryChannel::AbsorbedCount()` (delivery_channel.hpp:105). Domain 3's principle: "a contained failure that increments nothing is indistinguishable from success" (sub:432-436) | **DEFER-BUT-REGISTER**. The two places answer differently. L10 (d7:144-145) only requires swallowing, so this is not D7F scope. It needs an owner ruling (a new law or an accepted difference) |

### B.1 Proposed first entries — `docs/domains/bounded-context-register.md` (proposal text; file not created)

```
# Bounded-context register
One row per question the running system answers where two answers would be a defect.
Columns: id · question · single site · structural test · status · owner domain.

BC-1 · May a batch be handed to the callback now, and is it next in window order? · RecordBatchBatcher's one
       delivery function (the sole cb_ call), ordered by a window sequence assigned under mu_ ·
       SubscriberArrowBatchTest.CallbacksNeverOverlapAndArriveInWindowOrder (stress, in-flight ≤ 1, first-row
       strictly increasing) · DEFINED by D7F · d7 (L12)
BC-2 · Is this row delivered, dropped or neither? · RecordBatchBatcher's one classify-and-count function
       (NoteDropped folded in) · SubscriberArrowBatchTest.RowsAreConservedAcrossEveryEndingPath · DEFINED by D7F ·
       d7 (L1, L2). Registered gap: per-row SubscriberArrow::Subscribe drops silently on no codec (cpp:309-311).
BC-3 · Which assembly state is this subscription in; is the schema usable? · RecordBatchBatcher's state
       (awaiting-schema / assembling / undecodable / closed), fed by one usability decision ·
       SubscriberArrowBatchTest.EveryStateReportsDropsAtTimeoutAndClosing · DEFINED by D7F · d7 (L1, L6, L8)
BC-4 · When does a window close, and why? · RecordBatchBatcher::Flush · existing FlushesAt* tests + the BC-2
       property · REUSED · d7 (L4, L5). Registered: kRowLimit covers two causes (Q4).
BC-5 · After Unsubscribe returns, may anything still be delivered; lock order · row side: Subscriber
       RetireAndDrain (domain 3); batch side: RecordBatchBatcher::Stop. Order: batcher mu_ < batch-delivery
       serialization; neither held across Subscriber::Unsubscribe; gate < mu inside Subscriber ·
       UnsubscribeFromTimeoutCallbackWhileRowLimitInFlightDoesNotDeadlock (ctest TIMEOUT) · EXTENDED by D7F ·
       d3 + d7 (L9, L11)
BC-6 · Is this thread inside a delivery on object X? · core/internal/delivery_frame.hpp (InsideDeliveryOn /
       DeliveryScope); no other thread_local may answer it · existing CallerTier door tests + a batcher-token
       case · REUSED · d3 (shared)
BC-7 · Is a contained callback failure counted? · Subscriber: AbsorbedCallbackFailures; batcher: not counted ·
       none yet · DEFERRED — two answers, owner ruling owed · d3 / d7
```
