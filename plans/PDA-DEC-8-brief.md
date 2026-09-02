# PDA-DEC-8 — Stage Brief (2026-09-03)

**In one sentence:** an integrator can run two instances of the same protocol in one
application, on two domains with two different sets of settings, and it is now
*proved* they exchange no messages and share no topic declarations or settings.

**Forcing test:** `Registry.TwoInstancesTwoDomainsStayIsolated` — two instances,
selected the way an operator selects one, publishing on **the same topic name at
the same time**; each subscriber gets only its own messages, shape and size limit.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| *(none)* | — | Nothing added or altered. This stage proves a property the product already has; needing new surface to show the absence of shared state would mean the property was missing. |

## Deleted
Nothing — no bridge, setting or superseded test to retire. The stage adds a guard and the evidence that the guard can fail.

## Corner cases forbidden vs handled
**Forbidden:** a "mostly isolated" result (message logs are compared whole, so no
leakage tolerance exists to configure) · a proof that passes because the two
instances were never wired up to reach each other (a same-domain control asserts
messages *do* cross, and fails the moment the arrangement loses its teeth) · a test
that quietly skips when the transport is unavailable · "something failed" as a pass.
**Handled:** proving a message *never* arrives is undecidable in finite time, so the
claim is bounded by the window in which the control measured a real crossing, and
published with that bound · the vendor keeps a process-wide object we must route through, so the claim is scoped, not absolute.

## Decisions for you   (3)
1. **Two instances on the *same* domain with the same topic names — one shared
   message stream, or is the second refused?** (a) shared, as a DDS operator
   expects · (b) refuse the second.
   **Recommendation:** (a) — refusing needs a process-wide list of domains in use,
   the exact shared state this stage proves absent, and would forbid the primitive
   a future protocol bridge is built from. **Default:** (a). *Background: under
   (a) the same-domain case becomes the standing control.*
2. **Which protocols carry the "two instances do not interfere" claim?** (a) Fast DDS only · (b) Fast DDS and XRCE-DDS · (c) also the in-process loopback.
   **Recommendation:** (a) — the only protocol here where two instances in one
   application can genuinely reach each other, so the only one where the claim is
   measured rather than assumed; the loopback has no cross-instance path and XRCE
   would be testing the broker. **Default:** (a), narrowing written down.
3. **How wide is the isolation claim we publish?** (a) one application on one machine,
   with what is *not* covered written into the suite's docs · (b) spend a cross-machine harness to widen it.
   **Recommendation:** (a) — two separate processes cannot share the in-memory
   state this stage exists to disprove, so a wider harness costs maintenance and
   adds no evidence about the actual risk. **Default:** (a).

## Risks accepted / debt carried
- The forcing test passes against today's build, because the product already has the
  property. The real gate is six deliberate one-line breakages, each of which must be *observed* to fail the test and the observation recorded.
- The isolation claim is bounded, not absolute, and is published with its bound.
- If the suite's exclusivity lock were removed the concurrent case flakes first; the remedy is restoring the lock, never widening the timing window.

## Numbers
Declared net lines: +460 / −0 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
