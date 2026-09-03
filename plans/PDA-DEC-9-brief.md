# PDA-DEC-9 — Stage Brief (2026-09-03)

**In one sentence:** the pub/sub contract becomes a signed, frozen document — what is
fixed, what may only grow, how each of the six handoff conditions was actually checked, and
what the round's evidence does not cover — so the driver round and the language-binding
rounds can start the same day without asking each other anything.
**Forcing test:** a documentation review against the handoff checklist, plus one new
automated check that the published error-number table and the code can never disagree.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The interface specification | CHANGED — **frozen** | Two teams mirror it weeks apart; a moving contract is how they diverge silently |
| Published error-number table, shipped in the core package | NEW | Both bindings need the numbers in prose, in the package they consume |
| Architecture overview + top-level README | CHANGED | Two sentences describe how a subscriber gets a schema in a way that stopped being true mid-round |
| Decision log | NEW entry | Seam-as-contract and selection-by-name recorded as one architectural call |

No public code surface is added, altered or retired.

## Deleted
- The spec's migration count — its own recipe now returns 96 hits where it claims 18, because after the migration *everything* is configured that way. No replacement: the retired settings types exist nowhere, which the compiler checks for free.
- The record of earlier miscounts being corrected twice — one sentence replaces two paragraphs; the history stays in the progress log.
- The roadmap's promise to "pick a transport without compile-time coupling" — no replacement: that half shipped this round.

## Corner cases forbidden vs handled
**Forbidden:** free-floating counts (a number carries the command that derives it, or is not written); a second copy of the error-number table; a heading-exists "check" posing as verification; a handoff row with no verification method; any sentence readable as a platform claim; a third "negotiable" freeze class; a separate handoff document competing with the contract.
**Handled:** the unbounded-wait rule stays unpinned by any test — on this platform a correct and an incorrect implementation behave identically, so a test would pass for the wrong reason; the two known transport blind spots keep the owners your rulings gave them; Linux cannot be run from inside the round (decision 1).

## Decisions for you   (3)
1. **What should the handoff say about Linux, no automated build having ever run on this branch?**
   Options: (a) state the evidence exactly — all runs local on Windows plus one Linux compile — and tell both later rounds to treat Linux as unverified · (b) hold the handoff unsigned until you open the pull request and the lanes pass · (c) say nothing about platforms.
   **Recommendation:** (a) — matches the three times you chose a narrow claim stated honestly; (b) makes two rounds wait on one manual step.
   **Default if you don't answer:** (a)
   *Background (skippable): the lanes are `workflow_call` from a PR-triggered workflow, so opening the PR is the only trigger.*
2. **Must every guard the two later rounds add come with proof it can fail?**
   Options: (a) required — a guard ships with a recorded way it was made to fail, or it is not evidence · (b) recommended only.
   **Recommendation:** (a) — this round shipped seven guards that read as protection and could not fail; two teams who cannot consult each other will repeat it.
   **Default if you don't answer:** (a)
3. **While both rounds run, may either edit the frozen contract to record what it learns?**
   Options: (a) no — any change is a stop-and-ask to you, and each round writes findings in its own documents · (b) yes, into an append-only observations annex.
   **Recommendation:** (a) — under (b) two teams write into one file for weeks and the annex becomes a second, unreviewed contract.
   **Default if you don't answer:** (a)

## Risks accepted / debt carried
- The contract is frozen while its Linux behaviour is unproven; stated, not implied.
- Two of the six handoff conditions are verified by reading, not by machine — labelled as such rather than dressed up.
- Reconciliation covers the specification, the two top-level documents and the round's own completion claims; per-component document counts stay with the items that own them.

## Numbers
Declared net lines: +360 / −70 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
