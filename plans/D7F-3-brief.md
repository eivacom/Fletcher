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
No public type, method or signature changes. New public surface: 0.

## Deleted
- The check that dropped an oversized row uncounted (behaviour gone: it is counted); the duplicate
  "append or count lost" copy (one copy remains).
- Delivery from inside `Publish`, the timer or the unsubscribing thread (one batch thread instead);
  three "schema decided?" records (one, which refuses a second decision).
- No test is deleted. Ten tests that read results straight after publishing now wait for delivery.
## Corner cases forbidden vs handled
**Forbidden:** two callbacks at once or out of order; a callback on the publishing or transport
thread; anything after the closing batch; the closing batch before a waiting batch; a batch delivered
inside the callback that unsubscribed; a batch arriving after the subscriber object's destructor
returned on another thread (it now waits, even for a subscription that unsubscribed itself).
**Handled:** a callback slower than the data keeps batches queued in memory (your accepted cost); a
timeout that passes while your callback runs closes when it returns (closing on time would need a
second thread or a clock read per row on the transport's thread; nothing is lost).

## Decisions for you
1. **If your code destroys the subscriber object from inside one of its own batch callbacks, do the waiting batches and the closing batch still arrive?**
   (a) Yes, after that callback returns, as with an in-callback unsubscribe. You see every row, but batches arrive after the destructor returned, so your callback must outlive them. *Arch:* extends L9's exception from "Unsubscribe" to "Unsubscribe or destruction"; keeps L1 and L12.
   (b) No; they are discarded. You never hear of those rows. *Arch:* breaks L1 and the round purpose ("no lost row goes unreported").
   (c) The closing batch runs inside the destructor. Your callback is re-entered while running. *Arch:* breaks L12 (one at a time).
   **Recommendation:** (a): it is the choice you made for the same conflict with Unsubscribe, it keeps L1 and L12, and it needs no extra code. **Default:** (a), built as designed. L9's text names only Unsubscribe today.
2. **If your code unsubscribes a batched subscription from a thread that is delivering rows (e.g. a per-row callback), should Unsubscribe wait for that subscription's last batches?**
   (a) Wait. On return nothing more arrives and you may free your state; the transport thread pauses meanwhile. *Arch:* holds L9; domain 7's note "the transport's thread never waits on a callback" is reworded to "batch assembly never makes it wait".
   (b) Return at once; batches follow later. Freeing your state on return becomes unsafe. *Arch:* keeps that note literally; breaks L9.
   **Recommendation:** (a), from L9 and the round purpose; your ruling's words were about rows queuing, not this call. **Default:** (a). Today this call runs the closing batch on the transport thread itself.

## Risks accepted / debt carried
- Two subscriptions whose callbacks each unsubscribe the other wait forever (true today too; recorded).
  DEBT-6 is settled here, not carried.

## Numbers
Declared net lines: +90 / −60 (production) · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
