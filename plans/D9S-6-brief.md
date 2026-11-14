# D9S-6 — Stage Brief (2026-11-14)

**In one sentence:** On the server build, a window the shim fails to hand over still reaches the program, once,
as "no batch could be built", with its rows counted as lost and its reason unchanged, including "closing".
Until now such a window vanished uncounted.
**Forcing test:** `handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`. It fails the 2nd window,
then separately the closing one. Every window still arrives, exactly one closing delivery comes last, and rows
received plus rows lost equals rows published.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The "no batch could be built" delivery | CHANGED (meaning widens) | It now also covers "the shim could not hand this window over" (d9 L18) |
| The failure that comes with that delivery: why, plus a new origin code "hand-over" | NEW under (b), the default | d9 §1: every failure reaches the program as a status plus a message |
| A test-only fault shim, never packaged | NEW (test-only) | The only way to make a hand-over fail on demand. It is not public surface |

## Deleted
- **The silent loss:** the catch-everything handler becomes the reported delivery; ruling 2026-11-13's interim allowance ends.
- **Under (a) only:** the planned failure message is never built. Disclosed here.

## Corner cases forbidden vs handled
**Forbidden:** a window delivered twice; a "no batch" delivery that still carries part of a batch; a lost or
changed reason (which would lose the end signal); a retry or a partial batch; the fault hook in any shipped
build, edge or server.
**Handled:** a callback that throws despite the rule. It is swallowed after delivery, so nothing is lost. It
can't be forbidden: the shim can't stop foreign code throwing, and crashing would be unruled behaviour.

## Decisions for you
1. **Domain 9 promises that every failure reaches the program as a status plus a message. Does that promise
   hold when the shim fails to hand a batch over, or do you rule an exception for this one failure?**
   **(a) Rule an exception.**
   - *Users:* the program sees only "N rows lost" and the window's reason, as it does when the batcher
     itself cannot build a batch, so an operator cannot tell why.
   - *Architecture:* matches domain 7's own no-batch, which carries no message (d7 L7, L10), and the letter
     of ruling D30. No new surface. d9 §1 is amended to record the exception.
   **(b) Keep the promise.**
   - *Users:* the program also gets the reason: "internal error: …", with origin "hand-over". An operator can
     tell a shim failure from a batcher failure.
   - *Architecture:* keeps d9 §1, and d9 L2's "a failure is a status plus its message". The failure is
     borrowed for the callback, not owned (L11), so L2 gains that clause. It adds 1 field and 1 origin code
     (2 of the 3 surface slots). Only the plan's "no batch-layout change after stage 4" line is amended.

   **Recommendation:** (b). d9 §1 is a principle, and it outranks the details that point to (a): L18's silence
   on a message and the plan's layout line. d9 L2, L18's note and rung 8 point to (b). **Default:** (b).
   *Background (skippable): this settles d9 Q12's last half, which origin a failed hand-over carries. It is
   a new code, 4, under (b), and none under (a).*

## Risks accepted / debt carried
- The release of a half-built batch is proven by the code's shape, not measured; the leak-check leg is owed.
- CI runs about 20 s longer: the batch suite reruns, unarmed, against the fault shim.
- The fault shim stays out of packages by its file name only; nothing checks the package contents.

## Numbers
Declared net lines:
- (b): production +50 / −16; test-only fault shim +75; tests +130.
- (a): production +34 / −16; fault shim +75; tests +110.
New public surface: 2 under (b), 0 under (a). Design cycles used: 2/2.

---
*As landed:* (PM, at close)
