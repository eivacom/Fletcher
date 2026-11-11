# D7F-3 — Stage Brief (2026-11-11)

**In one sentence:** your batch callback runs only on your subscription's own thread, one batch at a
time, in arrival order; publishing never waits on it, unsubscribing delivers everything still
waiting before the closing batch, and an oversized row is always counted as lost.
**Forcing tests** (rungs 3–6):
- `EveryCallbackRunsOnTheBatchThreadInWindowOrder`: publishing returns while a callback is held;
  batches arrive on one other thread, one at a time, in order.
- `ClosingFromAnotherThreadDeliversQueuedWindowsThenClosing`: unsubscribing (or destroying) returns
  only after every waiting batch, then the closing batch, has arrived; nothing comes after.
- `OverBudgetRowAroundInCallbackUnsubscribeIsCounted` (`heavy`, ~256 MiB): the oversized row is
  reported as 1 lost; today it vanishes.
- `UnsubscribeFromInsideACallbackNeverDeliversNested`: returns at once; waiting batches, then the
  closing batch, follow after the callback returns, never inside it.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Which thread runs the batch callback | CHANGED | Always the subscription's own thread (your ruling "Always Fletcher's batch thread"); before, it could be the publisher's, the timer's or the unsubscriber's |
| What `Unsubscribe` waits for | CHANGED | From elsewhere: every waiting batch, then the closing one. From inside a callback: returns at once; they follow after the callback (your L9 rulings) |
| Header/README text; default test run | CHANGED | States the two rows above; the 256 MiB test runs only under `heavy` (your ruling) |
No public type, method or signature changes. New public surface: 0. Under Decision 2 (b) the header gains one private member, and its `Unsubscribe` text, the README and law L9 state the new "may not free on return" case.

## Deleted
- The check that dropped an oversized row uncounted (behaviour gone: it is counted); the duplicate
  "append or count lost" copy (one copy remains).
- Delivery from inside `Publish`, the timer or the unsubscribing thread (one batch thread instead);
  three "schema decided?" records (one, which refuses a second decision).
- No test is deleted. Ten tests that read results straight after publishing now wait for delivery.
## Corner cases forbidden vs handled
**Forbidden:** two callbacks at once or out of order; a callback on the publishing or transport
thread; anything after the closing batch; the closing batch before a waiting batch; a batch delivered
inside the callback that unsubscribed; an unsubscribe racing another one, or the destructor,
returning before the last batches arrived; a row joining a batch whose timeout has already passed.
**Handled:** a callback slower than the data keeps batches queued in memory (your accepted cost).

## Decisions for you
1. **If your code destroys the subscriber object from inside one of its own batch callbacks, do the waiting batches and the closing batch still arrive?**
   (a) Yes, after that callback returns, as with an in-callback unsubscribe. You see every row, but batches arrive after the destructor returned, so your callback must outlive them. *Arch:* extends L9's exception from "Unsubscribe" to "Unsubscribe or destruction"; keeps L1 and L12.
   (b) No; they are discarded. You never hear of those rows. *Arch:* breaks L1 and the round purpose ("no lost row goes unreported").
   (c) The closing batch runs inside the destructor. Your callback is re-entered while running. *Arch:* breaks L12 (one at a time).
   **Recommendation:** (a): it is the choice you made for the same conflict with Unsubscribe, and it keeps L1 and L12. **Default:** (a), built as designed. L9's text names only Unsubscribe today.
2. **If your code unsubscribes a batched subscription from a thread that is delivering rows, e.g. inside a per-row callback, should that call wait for the subscription's last batches?**
   *Fact, not asked:* destroying the subscriber object from such a thread is already forbidden (your ruling of 2026-09-05, interface spec §6 clause 5) and stops the program under either option.
   (a) Wait. On return nothing more arrives and you may free your state, but that thread holds the in-process transport's lock while waiting: if any of those batch callbacks calls Publish, Subscribe or Unsubscribe on that transport, both threads hang forever with no error. *Arch:* holds L9's main clause; contradicts the published rule that an Unsubscribe from inside a delivery does not wait, because waiting lets two handlers hang one another (interface spec §7 clause 6, ruling 2026-09-04); the design's premise P1 fails.
   (b) Return at once; the remaining batches, then the closing batch, still arrive in order on the subscription's own thread. No hang on the subscription's own transport (a thread delivering for a different transport instance still waits, as in domain 3), but you may not free what those callbacks use until the closing batch has arrived. *Arch:* extends domain 3's in-delivery carve-out to L9 (L9's exception widens); keeps L1 and L12.
   **Recommendation:** (b): the round purpose is that every batch arrives and no lost row goes unreported, and a hang delivers neither; §1 Intent leaves subscription lifetime to domain 3, whose rule (b) is. **Default:** (b). Fact: today's tree already hangs here for a timer-thread callback.

## Risks accepted / debt carried
- Two subscriptions whose callbacks each unsubscribe the other wait forever (true today; recorded). DEBT-6 settled.

## Numbers
Declared net lines: +92 / −64 (production; +6 more and one private header member under Decision 2 (b)) · new public surface: 0 ·
design cycles used: 2/2

---
*As landed (2026-11-11):*
Code +137/−81 (net +56) vs declared +98/−64: adds +40%, under the >50% line; ceiling 250. Owner rulings: Decisions 1 (a), 2 (b); rungs 6(d), 6(e) added. Not predicted: S1 guard (failed Subscribe stops its thread) + its test; S3 release at loop end; L1/L11/§1 term rename (no meaning change).
Fix cycles 2; implementer launches 4/5; design cycles 2/2. Open, recorded in BC-5: mutual-unsubscribe deadlock (pre-existing, documented); N2 domain-3 question.
