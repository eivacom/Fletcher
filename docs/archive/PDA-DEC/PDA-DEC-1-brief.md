# PDA-DEC-1 — Stage Brief (2026-09-01)

**In one sentence:** the delivery promises Fletcher makes to every subscriber — schema
before data, no reordering across the schema handoff, no delivery after unsubscribe —
stop being prose and become a suite run against all three protocols, two of them
**across a process boundary**, where transport behaviour is visible at last.
**Forcing test:** `ProviderConformance.SchemaBeforeDataAcrossHandoff` — a subscriber
joining before any publisher exists still gets every row, in order, with a schema
always in hand, never a row from "now" ahead of one from "before".

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Conformance suite (`integration-tests/pubsub-conformance`), plus its own CI lane | NEW | The contract becomes runnable; a new protocol inherits it by registering, not by copying tests. |
| Declaring a topic with a shape conflicting with an existing one | CHANGED — now **always** an error | Your ruling: the written contract tightens from "may be refused" to "must be refused". The loopback stops silently overwriting; the edge protocol gains the check. |
| The gateway's built-in loopback protocol | MOVED, and exercised here only as a **schema-less** transport | The suite must reach it; carrying schemas needs plumbing a later stage replaces, so it is not built twice. |

## Deleted
The gateway's private copy of the loopback protocol (replaced by the shared one), and
one Fast DDS test that checked late-joiner replay **within one process** (replaced by
the same check run in-process *and* across processes, on the new CI lane).

## Corner cases forbidden vs handled
**Forbidden:** a test that claims to cross a process boundary but quietly does not ·
"some rows arrived" as an acceptable outcome · a protocol quietly excluded from a run ·
a test that reads wire bytes · a difference recorded as "known" instead of fixed.
**Handled:** waiting for asynchronous arrival, since the contract promises subscribe
never blocks, so waiting *is* the behaviour under test (one bounded deadline, no
sleeps); and a transport carrying no schemas, which the contract explicitly allows.

## Decisions — all three answered, nothing open
1. **A conflicting topic declaration is refused by every protocol** (your ruling,
   2026-09-01), and the written contract is tightened to say so in this same change.
2. **Partial late-joiner delivery is never acceptable** — a transport replays all
   retained rows or none; anything between fails.
3. **A protocol the suite cannot exercise fails the run loudly**, naming what is
   missing, rather than skipping quietly as today's tests do.

## Risks accepted / debt carried
- Size is **not knowable up front**, deliberately: every difference the suite finds
  between the three protocols is fixed here, and they have never been compared
  mechanically before. The count is reported the first time it runs.
- One such difference could need a change to the wire bytes — a hard stop, back to you.
- The full suite is ~15 min slower on a fresh machine (a third-party helper service,
  built once and cached, shared with an existing suite).
- The loopback is not checked as a schema-carrying transport until a later stage adds
  the plumbing for it; recorded there so it is not forgotten.

## Numbers
Net lines +1750 / −115 (excluding the unknowable protocol fixes) · new public surface 1 ·
design cycles 2/2

---
*As landed (2026-09-01):*
**+3652 / −201** vs declared +1750/−115 — the overrun is ordered work under-costed
(the mirrored CI lane, the two-platform pipe helper, three divergence fixes not one).
Unpredicted: `InProcessProvider`'s lift moved here from PDA-DEC-5; the loopback ships
schema-**less** (its schema arrival is PDA-DEC-3's, with the 6th subject); XRCE gained
two provider fixes and a missing `ws2_32`. **The falsification never went red** — the
suite guards the contract but not that defect class, owned now by PDA-ABI-7 (ruling
2026-09-01). Design cycles 2/2 · fix cycles 2 · implementer launches 3/5.
