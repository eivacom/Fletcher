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
| Conformance suite (`integration-tests/pubsub-conformance`) | NEW | The contract becomes runnable; a new protocol inherits it by registering, not by copying tests. |
| The gateway's built-in loopback protocol | MOVED (behaviour identical) | The suite must reach it; today it is buried inside the gateway executable. |

## Deleted
The gateway's private copy of the loopback protocol (replaced by the shared one), and
one Fast DDS test that checked late-joiner replay **within one process** (replaced by
the same check run in-process *and* across processes).

## Corner cases forbidden vs handled
**Forbidden:** a test that claims to cross a process boundary but quietly does not ·
"some rows arrived" as an acceptable outcome · a protocol quietly excluded from a run ·
a test that reads wire bytes · a difference recorded as "known" instead of fixed.
**Handled:** waiting for asynchronous arrival, since the contract promises subscribe
never blocks, so waiting *is* the behaviour under test (one bounded deadline, no
sleeps); and a transport carrying no schemas, which the contract explicitly allows.

## Decisions for you
1. **When a second publisher claims a topic with a different data shape, what does it
   see?** (a) every protocol refuses it with an error · (b) each keeps today's
   behaviour — the written contract permits either. **Recommendation / default:** a —
   a silent mismatch decodes into the wrong fields with no error anywhere.
   *Background: §7 clause 3 says "may" be rejected; (a) means PDA-DEC-9 makes it "must".*
2. **A subscriber joins after rows were published. Is "some of them arrived" ever
   acceptable?** (a) no — a transport replays all retained rows or none · (b) yes,
   accept whatever each transport does. **Recommendation / default:** a — partial
   silent loss is the defect this round already shipped once; (b) hides it.
   *Background: all-or-nothing lets one check cover the loopback (retains nothing) and Fast DDS (retains everything) identically.*
3. **A protocol the suite cannot exercise here — the edge one needs a helper service
   — report or ignore?** (a) fail the run and name what is missing · (b) skip silently,
   as today's tests do. **Recommendation / default:** a — a silent skip certifies a
   protocol on no evidence, and the suite launches its own cached helper.

## Risks accepted / debt carried
- Size is **not knowable up front**, deliberately: every difference the suite finds
  between the three protocols is fixed here, and they have never been compared
  mechanically before. The count is reported the first time it runs.
- One such difference could need a change to the wire bytes — a hard stop, back to you.
- The full suite is ~15 min slower on a fresh machine (a third-party helper service,
  built once and cached, shared with an existing suite).

## Numbers
Net lines +1700 / −70 (excluding the unknowable protocol fixes) · new public surface 3 · design cycles 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
