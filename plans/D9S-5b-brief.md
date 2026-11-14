# D9S-5b — Stage Brief (2026-11-14)

**In one sentence:** however a batched subscription is closed (from outside, from inside its own
callback, or by destroying the subscriber), it ends with exactly one final "closing" delivery, even
when nothing was waiting, so a binding knows when it may free what its callback uses.
**Forcing test:** `Batches.EveryCloseEndsWithExactlyOneClosingDelivery`. Every way of closing an
idle subscription on the server build ends with one closing delivery, last. That delivery is an empty
table of the topic's layout once the subscription has decoded a row. Before its first row, even on a
declared topic, it is "no batch could be built", with nothing counted lost.
**Second forcing test:** `Batches.ARefusedBatchedSubscribeNeverDelivers`. A batched subscribe that is
refused never receives anything, even on a busy topic and even when refused inside another delivery.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Server build: closing a batched subscription, and destroying the subscriber | CHANGED (behaviour; no signature) | Always ends with exactly one closing delivery; the header promises that end signal instead of warning there is none |
| C++ batcher: unsubscribe and destruction | CHANGED (behaviour; no signature) | An idle close now delivers one empty closing batch (your ruling of 2026-11-14) |
| Batched subscribe (server build and C++) | CHANGED (behaviour; no signature) | A refused call never delivers, so its state may be freed on return; no batch arrives before the call returns (your ruling "(a) Never") |

## Deleted
- The test asserting "an idle close delivers nothing" (your ruling forbids it). The header's "no
  reliable end signal" and "must not destroy from inside a batch callback" warnings are replaced.

## Corner cases forbidden vs handled
**Forbidden:** two closing deliveries for one subscription; any delivery after the closing one; any
delivery at all for a refused subscribe; a delivery during an idle stretch while open (unchanged).
**Handled:**
- Building the empty batch fails (out of memory): the closing arrives as "no batch could be built".
  *Memory exhaustion cannot be ruled out.*
- Until D9S-6, a closing delivery the shim fails to convert is lost. *That is your interim of 2026-11-13.*

## Decisions for you
1. **When a batched subscribe is refused, may the caller still receive batches for it?** **Answered
   2026-11-14: "(a) Never (Rec.)"** (rulings ledger, "D9S-5b: a refused batched subscribe never
   delivers"). A refusal means nothing ever arrives, so the caller frees its state when the call
   returns. The few rows caught in the instant before a late refusal are discarded uncounted. You
   widened the round by that one batching-rule clause ("no silent loss" gains this exception).

## Risks accepted / debt carried
- C++ users get one extra empty closing batch on an idle close, and no batch before subscribe
  returns. A callback whose captured state dies before the subscriber now crashes even when idle (the
  README forbids it; 11 tests did it, fixed here).
- The refused-subscribe test relies on a timing race through the shim. If it cannot be seen failing
  before the change, a deterministic batcher-level test carries that proof, and the close reports it.

## Numbers
Declared net lines: production +20 / −4 (batcher), +0 / −0 (shim); tests ~+470 / −60 ·
new public surface: 0 · design cycles used: 2/2

---
*As landed:* (PM, at close)
