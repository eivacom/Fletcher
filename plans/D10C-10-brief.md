# D10C-10 — Stage Brief (2026-11-16)

**In one sentence:** when the native library cannot hand a window over, the C# handler still gets it, empty with
its rows counted, its reason, and the error (status, origin "hand-over", message), so it is told apart from data loss.
**Forcing test:** `AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure`. A test build fails one hand-over
(a middle window; the final window at close). Every other window arrives with no error, and no row goes uncounted.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The batch status a C# handler receives | NEW: an optional failure (status, origin, message), empty unless the hand-over failed | Owner ruling (2026-11-15), "Hand-over: Status carries the failure"; domain 10 names it |
| The .NET test lanes (Linux, Windows) | CHANGED: also build the test-only failing library and run one new test project against it | The failure can only be provoked by that test build; nothing shipped changes |

## Deleted
Nothing (since D10C-7 the window already arrives empty, rows counted; this adds the error).

## Corner cases forbidden vs handled
**Forbidden:** C# reporting a failure the library did not; rewriting its status or origin; freeing or keeping the
library's borrowed error (the handler gets a copy); a failed final window not ending the subscription.
**Handled:** copying the error runs out of memory: the window still arrives, rows counted, no error attached
(an allocation failure cannot be ruled out; delivering the window outranks its reason).

## Decisions for you
1. **Confirm the spec: when C# itself fails to take over a window (in practice only when out of memory), the handler
   gets the rows-lost count only, with no error attached?** Domain 10's law L9 already records this; no ruling covers it.
   (a) Count only, as the spec says. *Users:* a native failure arrives with its error; this rare C#-side loss arrives
   empty with rows counted and no reason, like "no schema yet". *Architecture:* within ruling :551 (3)'s scope (the
   library's hand-over); L9's "nothing lost silently" is met by the count; nothing changes.
   (b1) Attach an error tagged "hand-over". *Users:* one check covers every loss between batcher and handler, same
   shape. *Architecture:* C# reuses the library's hand-over code for its own failure, which strains L2 (C# authors no code).
   (b2) Attach C#'s own error as it is. *Users:* the reason arrives, but handlers must check the error's kind before
   reading status and origin, for every failure. *Architecture:* L2 holds, but the new field's public type widens.
   (b1, b2): no outside trigger, so only an internal test proves them, and this rung widens.
   **Recommendation:** (a): :551 (3)'s scope plus d10 L9 (:287-288), which already records (a); a safe default.
   **Default if you don't answer:** (a).
   *Background (skippable): D10C-7's take-over `catch`; `BatchStatus.Failure` = `FletcherException?`/`Handover` (b1), `Exception?` (b2).*

## Risks accepted / debt carried
- No edge-package test (this failure exists only on the server package). The message is checked present, not word
  for word (C++ runtimes differ); byte-exactness and "C# never frees the borrowed error" are proven internally only.
- Local full-solution runs need the test-only library (as copy-oracle already does); CI: +1 native build per lane.

## Numbers
Declared net lines: +90 / −4 (range +70..+120), 5 counted files: **not** light-lane · new public surface: 1 · cycles: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
