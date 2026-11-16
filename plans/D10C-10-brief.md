# D10C-10 — Stage Brief (2026-11-16)

**In one sentence:** when the native library cannot hand a closed window of rows over, a C# app's batch handler
still receives that window, told that rows were lost, why the window was closed, and the error itself (its
status, "hand-over" as the place it happened, and the library's message). The app can then tell this failure
apart from ordinary data loss.
**Forcing test:** `AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure`. A test build of the native
library is made to fail one hand-over on purpose: a middle window, and separately the final window at close. The
test proves the handler gets that window empty with its rows counted and the error attached, every other window
arrives normally with no error, and every row offered is either delivered or counted as lost.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The batch status a C# handler receives | NEW: an optional failure (status, origin, message), empty unless the hand-over failed | Owner ruling (2026-11-15), "Hand-over: Status carries the failure"; domain 10 names it |
| The .NET test lanes (Linux, Windows) | CHANGED: also build the test-only failing library and run one new test project against it | The failure can only be provoked by that test build; nothing shipped changes |

## Deleted
Nothing. Since D10C-7 this window already arrives empty with its rows counted; this stage adds the error to it.

## Corner cases forbidden vs handled
**Forbidden:** C# reporting a failure the native library did not report. C# rewriting the error's status or
origin; they arrive exactly as given. C# freeing or keeping the library's borrowed error; the handler gets its
own copy. A failed final window ending the subscription differently; it still ends it.
**Handled:** if copying the error itself runs out of memory, the window still arrives, with its rows counted and no
error attached. An allocation failure cannot be ruled out, and delivering the window matters more than its reason.

## Decisions for you
1. **When C# itself fails to take over a window (practically only when the process is out of memory), should the
   handler also get the error, or only the rows-lost count it gets today?**
   (a) Only the count. *Users:* a native failure arrives with its error; this rare C#-side loss arrives as an
   empty window with rows lost and no reason, which looks like "no schema yet". *Architecture:* matches the
   ruling's scope (the library's hand-over) and law L2 (C# authors no error code); L9's "nothing lost silently"
   holds, because the rows are counted.
   (b) Attach the error too. *Users:* one check covers every loss between batcher and handler, with its reason.
   *Architecture:* C# would label its own failure with the library's "hand-over" origin, which strains L2. Nothing
   outside the code can trigger it, so only an internal test would prove it, and that widens this rung.
   **Recommendation:** (a). It follows L2 and the ruling as worded, and L9 already holds.
   **Default if you don't answer:** (a).
   *Background (skippable): the D10C-7 thunk's take-over `catch` (residue (a)); `BatchStatus.Failure` would become
   `Exception?` or reuse origin `Handover` = 4 under (b).*

## Risks accepted / debt carried
- No edge-package test: this failure exists only on the server package (the edge refusal is D10C-7's test).
- The message is checked as present and passed through, not word for word (C++ runtimes word it differently);
  that it arrives byte-exact, and that C# never frees the borrowed error, are proven by internal tests only.
- Running the whole .NET solution locally needs the test-only native library, as the copy-oracle project already
  does (documented). CI: both .NET lanes build one more native configuration (several minutes each).

## Numbers
Declared net lines: +90 / −4 (range +70 to +120) across 5 counted files, so this is **not** light-lane eligible ·
new public surface: 1 (one property on an existing type) · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
