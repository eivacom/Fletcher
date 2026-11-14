# D9S-6 — Stage Brief (2026-11-14)

**In one sentence:** On the server build, a window the shim fails to hand over still reaches the program, once,
as "no batch could be built". Its rows are counted as lost, its reason is unchanged (including "closing"), and
the program is told why. Until now such a window vanished uncounted.
**Forcing test:** `handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`. It fails the 2nd window,
then separately the closing one. Every window still arrives, and exactly one closing delivery comes last.
The failed window carries "internal error" with the origin "hand-over". Rows received plus rows lost equal
rows published.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The "no batch could be built" delivery | CHANGED (meaning widens) | It now also covers "the shim could not hand this window over" (d9 L18) |
| The failure carried with that delivery (status, message) | NEW | d9 §1: every failure reaches the program as a status plus a message |
| The origin code "hand-over" | NEW | Says the shim, not the batcher, failed (settles d9 Q12) |
| A test-only fault shim, never packaged | NEW (test-only) | The only way to make a hand-over fail on demand. It is not public surface |

## Deleted
- **The silent loss.** The catch-everything handler becomes the reported delivery, and ruling 2026-11-13's
  interim allowance ends.
- **d9 L18's "ambiguous" note.** Replaced by the settled shape.

## Corner cases forbidden vs handled
**Forbidden:**
- a window delivered twice;
- a "no batch" delivery that still carries part of a batch;
- a lost or changed reason, which would lose the end signal;
- a retry or a partial batch;
- the fault hook in any shipped build, edge or server.

**Handled:** a callback that throws despite the rule. The exception is swallowed after delivery, so nothing is
lost. It can't be forbidden: the shim can't stop foreign code from throwing, and crashing instead would be
behaviour no ruling grants.

## Decisions for you
None open. Decision 1 is answered: "(b) Keep the promise (Rec.)" (ruling "2026-11-14 — D9S-6: a failed
hand-over still carries its status and message"). The failure's message is borrowed for the callback; a
program copies it if it wants to keep it.

## Risks accepted / debt carried
- The release of a half-built batch is proven by the code's shape, not measured. The leak-check leg is still owed.
- CI runs about 20 s longer: the batch suite reruns, unarmed, against the fault shim.
- The fault shim stays out of packages by its file name only; nothing checks the package contents.

## Numbers
Declared net lines: production +50 / −16; test-only fault shim +75; tests +130.
New public surface: 2. Design cycles used: 2/2.

---
*As landed — 6, 2026-11-14:* production has 67 touched non-comment lines in 3 files (`batches_server.cpp`, `handles.hpp` for DEBT-5, `binding.h`), against a declared +50/−16. Tests and the seam: +236/−3, against a declared +205.
Decision 1 was ruled "(b) Keep the promise" (:504). The record fix deleted an unruled "throwing callback is swallowed" clause from L18 (4a F1). The fault hook stays test-only (M6 does not link the shipped shim). Cycles: design 2, fix 0, implementer launches 1 (resumed once).
