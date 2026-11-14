# D9S-5a — Stage Brief (2026-11-14)

**In one sentence:** On the server build, a C caller can close one batched subscription without
destroying its subscriber. Every batch Fletcher has already accepted arrives first, then the
closing batch, then nothing. Two existing promises are now also proven through the C surface: a
batch belongs to the caller only for its callback unless the caller takes it, and batch options
reach Fletcher's one batcher exactly as given.

**Forcing test:** `Batches.ClosingFromOutsideDeliversTheRestThenTheClosingBatch`. When the close
returns, every accepted batch has arrived, the closing batch last, and nothing arrives afterwards.
This holds whether the caller closes the subscription or destroys the subscriber, and a close
leaves other subscriptions running. Alongside it are an edge-build refusal check and two guards, one for
taking a batch and one for the options (42 cells, with a timing control).

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Close one batched subscription (server build) | NEW | Today the only way to stop batches is to destroy the whole subscriber |
| The same call on the edge build | NEW (refusal) | It answers "not supported" and names the server build, as the batched subscribe does |
| Header text: batched subscribe, cancel, destroy | CHANGED (text) | It points to the new close; no existing call answers differently |

## Deleted
Nothing. One header sentence is rewritten ("only destroying the subscriber ends it, for now").

## Corner cases forbidden vs handled
**Forbidden** (refused at the door, or impossible by construction):
- On the edge build, the close refuses and does nothing.
- An id that is not a batched-subscription id is refused and nothing is closed; otherwise a per-row
  id could silently close an unrelated batched subscription with the same number.
- The shim cannot decide when the closing batch comes: it has no timer, wait or thread.
- A batch the caller took is never released by Fletcher. One the caller left is released exactly
  once, after the callback returns.

**Handled:**
- **Closing an already-closed subscription does nothing.** It can't be forbidden: Fletcher's rule
  is that cancelling something no longer live is a no-op, so teardown can call it unconditionally.
- **An id is meaningful only on the subscriber that issued it.** It can't be forbidden: per-row ids
  already work this way, and checking it would need a registry of every issued id.
- **A close while idle, or from inside a callback, is not yet guaranteed a closing batch.** Your
  2026-11-14 ruling settles this, and D9S-5b builds it. This stage asserts nothing about it.

## Decisions for you
None. No existing call changes its answer. There is no new error code, no timer, window or range
check in the shim, and no change to the batcher, the encoding or C#.

## Risks accepted / debt carried
- **Left to D9S-5b:** a close from inside a callback, a close from a row-delivery thread, and a
  close while idle. All three are documented in the one place 5b rewrites.
- **"Nothing left behind" is proven by count, not by measuring memory.** Every batch handed over is
  released exactly once. Measuring the shim's memory would need a test hook inside the shipped
  library. A leak-checking CI leg would be stronger once Linux CI runs; it is registered, not built.
- **Files:** 5 production files, against the 2–3 estimated; the lines are within the estimate.

## Numbers
Declared net lines: production +80/−9 (≈+52/−6 counted, ≈89 touched) · tests +490/−3 · new public surface: 1 · design cycles used: 1/2

*As landed:* (PM, at close)
