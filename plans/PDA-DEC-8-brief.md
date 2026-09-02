# PDA-DEC-8 — Stage Brief (2026-09-03, revision 1)

**In one sentence:** an integrator can run two instances of the same protocol in one
application — on two domains, and with two different message-size limits — and it is
now *proved* they exchange no messages and share no topic declarations or settings.
**Forcing test:** `Registry.TwoInstancesTwoDomainsStayIsolated` — two instances chosen
the way an operator chooses one, publishing on **the same topic name** with **identical
size limits**, so only the domain could keep them apart; each gets only its own messages
and shape. A second case covers the size limit on its own instance pair.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| *(none)* | — | Nothing added or altered. This stage proves a property the product already has; needing new surface to show the absence of shared state would mean the property was missing. |

## Deleted
Nothing — no bridge, setting or superseded test; the stage adds a guard and its evidence.

## Corner cases forbidden vs handled
**Forbidden:** a "mostly isolated" result (message logs compared whole, so no leakage
tolerance exists) · a proof that passes because the instances could never have reached
each other (identical size limits, and a same-domain control asserts messages *do*
cross) · a test that quietly skips · "something failed" as a pass.
**Handled:** proving a message *never* arrives is undecidable, so the claim is bounded
by the window in which the control measured a real crossing · the vendor's process-wide
object cannot be removed, so the claim is scoped · a message over an instance's limit is
**discarded without an error** — long-standing behaviour, disclosed and measured.

## Decisions for you   (1)
1. **How wide is the isolation claim we publish?** (a) one application on one machine,
   with three exclusions in the docs — nothing about isolation between machines, vendor
   process-wide state, or the shared memory two *separate* processes on one machine
   use · (b) buy a cross-machine harness. **Recommendation:** (a) — separate processes
   cannot share the in-memory state this stage disproves, so a wider harness adds
   maintenance and no evidence; it matches the scope you chose twice. **Default:** (a).

**Already decided, not asked:** *two instances on the same domain with the same topic
names share one message stream* — spec §4 clause 3 ("no global state"): refusing the
second needs the process-wide list of domains in use that the clause forbids, and that
case becomes the standing control. *Fast DDS alone carries the claim* — the plan's story
for this stage ("two instances of the same provider, **on two DDS domains**, in one
process, through the registry"); the loopback keeps no cross-instance state at all.

## Risks accepted / debt carried
- The forcing test passes against today's build, because the product already has the
  property. The gate is six deliberate one-line breakages, each observed green
  immediately before it is applied, its failure recorded verbatim, and the machine's
  shared-memory scratch cleared after any crash — a red no different from an
  environment fault proves nothing.
- The isolation claim is bounded, not absolute, and published with its bound. If the
  suite's exclusivity lock were removed the concurrent case flakes first; the remedy
  is restoring the lock, never widening the timing window.

## Numbers
Declared net lines: +570 / −0 · new public surface: 0 · design cycles used: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
