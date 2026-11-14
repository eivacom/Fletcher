# D9S-5b — Stage Brief (2026-11-14)

**In one sentence:** however a batched subscription is closed (from outside, from inside its own
callback, or by destroying the subscriber), it now always ends with exactly one final "closing"
delivery, even when nothing was waiting, so a binding always knows when it may free what its
callback uses.
**Forcing test:** `Batches.EveryCloseEndsWithExactlyOneClosingDelivery`. Through the shim's server
build, every way of closing an idle subscription (outside or inside the callback, close or destroy,
with or without a known table layout) ends with one closing delivery, last, and nothing after it.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Server build: closing a batched subscription, and destroying the subscriber | CHANGED (behaviour; no signature) | Always ends with exactly one closing delivery; the header now promises that end signal instead of warning there is none |
| C++ batcher: unsubscribe and destruction | CHANGED (behaviour; no signature) | An idle close now delivers one empty closing batch (your ruling of 2026-11-14) |
| Edge build | unchanged | It has no batcher; its refusals stay as they are |

## Deleted
- The test asserting "an idle close delivers nothing". Behaviour gone, disclosed: your ruling
  forbids it.
- The header warnings "no reliable end signal" and "a binding must not destroy from inside a batch
  callback". Replaced by the guaranteed closing delivery.

## Corner cases forbidden vs handled
**Forbidden** (cannot happen by construction): two closing deliveries for one subscription; any
delivery after the closing one; a closing delivery for a refused subscribe call (the caller was told
"no subscription" and may already have freed its state); an empty closing reported as "no batch could
be built" when the table layout is known; a delivery during an idle stretch while open (unchanged).
**Handled:**
- If building the empty batch itself fails (out of memory), the closing still arrives, as "no batch
  could be built". *Memory exhaustion cannot be ruled out.*
- Until stage D9S-6, a closing batch the shim fails to convert is lost. *That is the interim you
  accepted on 2026-11-13; D9S-6 must keep it a closing delivery.*

## Decisions for you
None. What the empty closing delivery looks like is already settled by law. When the subscription
knows its table layout, the caller gets an empty table of that layout. When it never learned one, the
caller gets "no batch could be built", with nothing counted lost. These are the two shapes the
batching rules already use for a window that has losses but no rows.

## Risks accepted / debt carried
- C++ users of the batcher see one extra, empty closing batch when closing an idle subscription.
  Empty and "no batch" deliveries were already possible, so a correct callback already copes.
- Reported, not fixed: if a subscribe call fails after rows have already reached it, those rows
  still arrive as a closing delivery. This is practically unreachable, and fixing it would change
  more than your ruling allows.
- The shim's code is unchanged. Its laws require it to pass through what the batcher delivers, so
  only its header text changes.

## Numbers
Declared net lines: production code +12 / −3 (batcher), +0 / −0 (shim); tests ~+360 / −60 ·
new public surface: 0 · design cycles used: 1/2

---
*As landed:* (PM, at close)
