# D9S-5b — Stage Brief (2026-11-14)

**In one sentence:** however a batched subscription is closed (from outside, from inside its own
callback, or by destroying the subscriber), it ends with exactly one final "closing" delivery, even
when nothing was waiting, so a binding knows when it may free what its callback uses.
**Forcing test:** `Batches.EveryCloseEndsWithExactlyOneClosingDelivery`. Every way of closing an
idle subscription on the server build ends with one closing delivery, last. That delivery is an empty
table of the topic's layout once the subscription has decoded a row. Before its first row, even on a
declared topic, it is "no batch could be built", with nothing counted lost.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Server build: closing a batched subscription, and destroying the subscriber | CHANGED (behaviour; no signature) | Always ends with exactly one closing delivery; the header promises that end signal instead of warning there is none |
| C++ batcher: unsubscribe and destruction | CHANGED (behaviour; no signature) | An idle close now delivers one empty closing batch (your ruling of 2026-11-14) |

## Deleted
- The test asserting "an idle close delivers nothing" (your ruling forbids it). The header's "no
  reliable end signal" and "must not destroy from inside a batch callback" warnings are replaced.

## Corner cases forbidden vs handled
**Forbidden:** two closing deliveries for one subscription; any delivery after the closing one; an
*empty* closing for a refused subscribe call; a delivery during an idle stretch while open (unchanged).
**Handled:**
- Building the empty batch fails (out of memory): the closing arrives as "no batch could be built".
  *Memory exhaustion cannot be ruled out.*
- Until D9S-6, a closing delivery the shim fails to convert is lost. *That is your interim of 2026-11-13.*
- A refused subscribe that still delivers: see Decision 1.

## Decisions for you
1. **When a batched subscribe is refused, may the caller still receive batches for it, and when may it free what it handed in?**
   Today it can. A subscribe refused late (its topic options clash with an existing subscription to a busy topic, or the transport delivered once and then failed) passes on the rows it caught. The last of them can arrive after the refusal, and a binding that freed its state on refusal then crashes or corrupts memory.
   - (a) **Never.**
     - Users: a refusal means nothing ever arrives, so the caller may free its state as soon as the call returns. Batches also never arrive before subscribe returns.
     - Architecture: serves d9 §1 "who releases each" and matches every other refusal (L12, L15). It strains d7 L1 ("no silent loss"): the few rows caught are discarded uncounted, so you would widen the round by one more domain-7 clause. ~+18/−3 lines.
   - (b) **Sometimes.**
     - Users: after such a refusal a binding cannot know whether more will come, so it must keep its state for the life of the process. That is a small permanent leak per refusal, and the end signal there is unreliable.
     - Architecture: keeps d7 L1 and stays inside the round, but strains d9 §1 and your 2026-11-14 ruling's own reason ("a binding always knows when it may free its state"). +12/−3.
   - (c) **Always exactly one closing delivery, refused or not.**
     - Users: one rule, "free on closing". Even a plainly wrong call (bad argument, edge build) calls back once.
     - Architecture: uniform with 2026-11-14 and keeps L1, but it changes the edge build's answer (L12), contradicts d7 L15's "no delivery", and calls back off the batch thread (D10). ~+28/−2.
   **Recommendation:** (a). The round exists so that any binding can take batches through the shim, and that needs a rule for freeing state that a binding can follow (d9 §1).
   **Default if you don't answer:** (b), for this round only. It needs no widening, and the header warns. Nothing ships, and C# uses batches only from its switch round, where this question reopens.
   *Background: domain 3 registers the new subscription before its last checks. Moving those checks is domain 3's work, outside this round, and would not cover the transport-failure case.*

## Risks accepted / debt carried
- C++ users get one extra empty closing batch on an idle close. A callback whose captured state dies
  before the subscriber now crashes even when idle (the README forbids it; 11 tests did it, fixed here).

## Numbers
Declared net lines: production +12 / −3 (batcher, option b; a: +18 / −3; c: +10 / −2 plus ~+18 shim),
shim +0; tests ~+385 / −60 · new public surface: 0 · design cycles used: 2/2

---
*As landed:* (PM, at close)
