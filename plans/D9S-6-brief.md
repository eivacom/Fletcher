# D9S-6 — Stage Brief (2026-11-14)

**In one sentence:** On the server build, a window of rows that the shim fails to hand to the program still
reaches it. It arrives once, as "no batch could be built", with its rows counted as lost and its reason
unchanged, including "closing". Until now such a window vanished without being counted.
**Forcing test:** `handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`. It makes the hand-over
fail for the 2nd window, and separately for the closing window. The program must still receive every window,
the failed one marked as lost, with exactly one closing delivery and it last. Rows received plus rows reported
lost must equal rows published.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The "no batch could be built" delivery | CHANGED (meaning widens) | It now also covers "the shim could not hand this window over" (d9 L18) |
| A test-only fault shim, never packaged | NEW (test-only) | The only way to make a hand-over fail on demand. It is not public surface |

## Deleted
- **The silent loss.** The shim's catch-everything handler is replaced by the reported delivery. The interim
  allowance "lost uncounted during stages 4–5" (ruling 2026-11-13) ends here.
- **The planned failure message**, under Decision 1 (a): never built. Disclosed here.

## Corner cases forbidden vs handled
**Forbidden:** a window delivered twice; a "no batch" delivery that still carries part of a batch; a lost or
changed reason (which would lose the end signal); a retry or a partial batch; the fault hook in any shipped
build, edge or server.
**Handled:** a program's callback that throws despite the rule. The throw is swallowed after the window has
been delivered, so nothing is lost. It cannot be forbidden, because the shim cannot stop foreign code from
throwing, and crashing instead would be new behaviour that no ruling grants.

## Decisions for you
1. **When the shim fails to hand a batch over, is the program told why, or only that the rows were lost?**
   **(a) Only that the rows were lost.** *Users:* "N rows lost" plus the window's reason, the same as when
   the batcher itself cannot build a batch; an operator cannot tell a shim failure from a batcher failure.
   *Architecture:* the program gets the batcher's answer unchanged (round purpose: "batches straight from the
   one batcher"; d7 L7, L10), one error channel (d9 L2), no new surface. It strains d9 §1's "every failure
   becomes a status plus message", for this one failure.
   **(b) Also an error message.** *Users:* the program also gets "internal error: …", for shim failures
   only; batcher failures still arrive without one. *Architecture:* serves d9 §1's message promise, but
   strains d9 L2 (a second way for failures to reach the program), takes 2 of the 3 new-surface slots, and
   changes the batch layout the plan had fixed.
   **Recommendation:** (a), on the round purpose and d9 L2: the shim passes the one batcher's answer through
   and adds no failure channel of its own. **Default if you don't answer:** (a).
   *Background (skippable): the plan's rung 8 and a note under d9 L18 assume a message crosses; the ruling
   (D30) does not ask for one. Choosing (a) also settles d9 Q12's last half: no origin code, because no error
   value crosses.*

## Risks accepted / debt carried
- When a hand-over fails, the release of the half-built batch is proven by the code's shape, not measured.
  The leak-check leg is still owed (D9S-5a DEBT-1).
- CI runs about 20 s longer: the batch suite runs a second time, unarmed, against the fault shim, to show
  the hook changes nothing when off.

## Numbers
Declared net lines: production +34 / −16; test-only fault shim +75; tests +110 (stage about +219 / −16;
(b) adds about +25) · new public surface: 0 ((b): 2) · design cycles used: 1/2

---
*As landed:* (PM, at close)
