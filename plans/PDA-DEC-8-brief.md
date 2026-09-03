# PDA-DEC-8 — Stage Brief (2026-09-03, revision 1)

**In one sentence:** an integrator can run two instances of the same protocol in one
application and it is now *proved* that two instances on **different domains** exchange
no messages and share no topic declarations or settings, and — separately — that two
instances with **different message-size limits** each honour their own. Those are two
claims about two pairs, not one claim about one; folding them together is what review
debt C2-1 struck, because differing size limits stop the instances discovering each
other at all, so "exchange no messages" would be unearned there.
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
*As landed (2026-09-03, PM):* **+903 / −104** over `integration-tests/` + `docs/` across
both commits (`b7b33f3` +710/−25, `eb69297` +193/−79), vs declared +570/−0 — over on adds,
and the overrun is evidence rather than scope: **zero product code**, 592 lines of it one
test file. Public surface **0** as declared. All six mutation rows observed red with
verbatim text, independently re-run by compliance with Conan cache forensics. Not
predicted by the brief: a **dead guard** (`AwaitSubscriptionsLive` returned `kOk` on a
null schema — the round's seventh inert guard), a **real domain collision** (154 was
already `gateway-end-to-end`'s; both design cycles checked only the C++ suites, so the
range moved to 161–167 with the census command recorded), and the control's margin being
a **mislabelled quantity** — the crossing is 0 ms, not the ~260/279 ms two reviewers and
the PM attributed to it, which was the case's wall time. Round-close full suite green with
every component force-rebuilt. Design landed 309 lines vs a 300 cap; the +9 is mandated
provenance and I declined to trim it. Cycles: design 2/2 · fix 1 · implementer launches
2/5 · owner touches 1 (the claim-scope ruling).