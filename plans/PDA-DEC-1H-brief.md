# PDA-DEC-1H — Stage Brief (2026-09-02, written retrospectively)

**In one sentence:** the test harness that certifies XRCE conformance now proves the Agent
answering the port is the one it started, instead of trusting that the process it launched is
still alive.
**Forcing test:** `AForeignAgentDoesNotSatisfyTheHarness`, one copy in each XRCE harness — with
a stranger's Agent on the port, the harness must refuse to certify rather than run green.

**Why this exists at all:** a reviewer on the previous item watched the suite report a full pass
while served by an Agent nobody in the run had started. Every XRCE pass before this item was
therefore conditional on "no stray Agent happened to be listening" — including passes reported
to you earlier today.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Harness bring-up | NEW: proves the OS records *our* child as the port's holder | liveness was true-but-stale; a race let a leftover answer first |
| Harness bring-up | **DELETED**: the fall-back-and-pass path | it would have re-admitted this item's own defect behind one stdout line |
| Operator-facing failure message | CHANGED | it told operators to build a binary that already existed and never mentioned the port |

## Deleted
- The "cannot prove it, carry on" state. A platform with neither OS query now fails to
  **compile**; a query that fails at runtime **refuses**. Behaviour gone, deliberately.

## Corner cases forbidden vs handled
**Forbidden:** certifying a run whose Agent we cannot prove we own; a stranger sharing the port
(a foreign holder refuses even when ours is also present); an unsupported platform passing
silently; a guard an operator has to remember to switch on.
**Handled:** a stale process id that could be recycled; a probe that answers before our child
has died; a missing Agent binary (now distinguished from losing the port).
**Explicitly NOT claimed:** the proof is taken at bring-up, so a foreign Agent arriving
mid-run is not caught. Stated in both READMEs rather than implied away.

## Decisions for you   (none)
Nothing needed adjudicating. One decision was mine and is recorded: extending the fix to the
second XRCE harness, which carried the same defect in worse form (no liveness check at all, so
a leftover satisfied it outright). Your 2026-09-02 ruling was about not signing the handoff over
a harness that can report a vacuous green, and that reasoning covered both.

## Risks accepted / debt carried
- Bring-up-only scope, above.
- The ownership block is duplicated in the two harnesses, byte-identical. I first accepted a
  false justification for that (a CI sparse-checkout cost) and repeated it to you; the reviewer
  checked the workflow files and disproved it. Duplication stands for the real reason — sharing
  would export a test-only header from a shipped package — and both copies now say so.
- Left as bigger than this item: the 1 s POSIX reap window, structural child-process cleanup
  (job objects / `PR_SET_PDEATHSIG`), and an unbounded `waitpid`. All pre-existing.

## Numbers
Not declared up front (compressed cycle) · actual **+1263 / −246** · new public surface **0** ·
design cycles 0 (compressed) · fix cycles 1

---
*As landed (2026-09-02, PM):* closed the round's **only known false-green vector**. The PM
hypothesis about the mechanism was wrong and was refuted by measurement before any fix was built
— an Agent that cannot bind exits in ~876 ms; the real cause was a race with a probe answering
16 ms ahead of the doomed child's death. Cost of the compressed cycle, paid honestly: this brief
and the design doc were written *after* the work, and a compliance review was run at close rather
than skipped. conformance **82 ctest entries / 26 gtest cases in `conformance_xrce`**, 0 skipped;
interop **1 entry / 4 cases**, 0 skipped; no stray Agents across six checks.
