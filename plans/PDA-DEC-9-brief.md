# PDA-DEC-9 — Stage Brief (2026-09-03)

**In one sentence:** the pub/sub contract becomes a signed, frozen document — what is fixed, what may only grow
**and who may grow it**, how each handoff condition was really checked, and what the evidence does not cover — so
the driver round and the language-binding rounds can start the same day without asking each other anything.
**Forcing test:** a documentation review against the handoff checklist, plus one new check that makes it
**impossible to add an error cause without publishing it**: the build fails.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The interface specification | CHANGED — **frozen** | Two teams mirror it weeks apart; a moving contract is how they diverge silently |
| Published error-number table, in the repository beside the code defining the numbers | NEW | Both bindings need the numbers in prose; a test reads that file, so the two cannot disagree |
| Architecture overview + top-level README | CHANGED | Two sentences describe how a subscriber gets a schema in a way that stopped being true mid-round |
| Decision log | NEW entry | Seam-as-contract and selection-by-name recorded as one architectural call |

## Deleted
- The specification's migration count *and* the per-file table under it — the count's own recipe returns 96 hits where it claims 18 (after the migration, *everything* is configured that way), the table does not reproduce either, and its rows do not add up to its own heading. No replacement: a count nobody can re-derive is deleted, not dated, and the retired settings types exist nowhere, which the compiler checks for free.
- The record of earlier miscounts being corrected twice — one sentence replaces two paragraphs; the history stays in the progress log.
- From the roadmap, only the claim that picking a transport at run time is still future — it shipped. The promises of *loadable* plugins and of not linking a transport's library both stay: neither shipped.

## Corner cases forbidden vs handled
**Forbidden:** any count in the contract a machine cannot re-derive on demand; a second copy of the error-number
table; a guard that cannot fail on the change it claims to catch; a "check" that is only a heading being present;
a handoff row promising verification without naming it; any sentence readable as a platform claim; a third,
negotiable class of contract text; a rival handoff document beside the contract.
**Handled:** the unbounded-wait rule stays unpinned by any test — a correct and an incorrect implementation are
indistinguishable on this platform; the two known transport blind spots keep the owners your rulings gave them;
Linux cannot be exercised from inside the round (decision 1); and one condition — nothing above the seam can tell
a built-in transport from a loaded one — is held by the freeze, not a machine, since any machine check would have
to add the very distinction the round removes.

## Decisions for you   (1)
1. **What should the handoff say about Linux, no automated build having ever run on this branch?**
   Options: (a) state the evidence exactly — all runs local on Windows plus one Linux compile — tell both later rounds to treat Linux as unverified, and make a Linux-only difference in seam behaviour a question for you rather than a local fix · (b) hold the handoff unsigned until you open the pull request and the lanes pass · (c) say nothing about platforms.
   **Recommendation:** (a) — matches the three times you chose a narrow claim stated honestly; (b) makes two rounds wait on one manual step.
   **Default if you don't answer:** (a)
   *Background (skippable): the lanes are `workflow_call` from a PR-triggered workflow, so opening the PR is the only trigger.*

**Already decided, not asked:** *a guard may ship with a recorded blind spot* — your 2026-09-01 ruling "Ship the
guard, hunt elsewhere"; the contract restates its existing wording unchanged and does **not** make falsification
a must-pass gate for the later rounds, which would forbid the outcome you chose · *neither later round may edit
the frozen contract* — locked decision 1 and specification §1 · *who may add a new error cause* — the same rule:
you allocate it, so two rounds cannot both take the same number.

## Risks accepted / debt carried
- The contract is frozen while its Linux behaviour is unproven; stated, not implied.
- Only **two of six** handoff conditions are machine-checked end to end; three are checked by
  a named reader on a named date and one by construction — the honest count after review.
- Reconciliation covers the specification, the two top-level documents and the round's own
  completion claims; per-component document counts stay with the items that own them. No
  public code surface is added, altered or retired, and the package's **contents** do not change.

## Numbers
Declared net lines: +330 / −100 · new public surface: 0 · design cycles used: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
