# PDA-DEC-A1 — Stage Brief (2026-09-04)

**In one sentence:** an application Fletcher hands a send buffer can now write its row
straight into the bytes the subscriber will read, instead of building it somewhere else and
having it copied in — and the zero-copy guard can, for the first time, tell the two apart.

**Forcing test:** `CopyAccounting.InPlaceEncodeWritesIntoTheDeliveredWindow` — proves the
bytes a client composes are, at the same address, the bytes that come out the other end.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| "fill the send buffer in place" | NEW | today a client can only hand over bytes it already has; there is no way to be *given* the space |
| the published zero-copy promise for rows | CHANGED | it covers only the half of the path inside Fletcher — decision 1 |
| the zero-copy guard's measured interval | CHANGED | it begins after the client has finished writing, so it cannot see a copy made there |

## Deleted
- The guard's written exclusion of "the encode itself" from what counts as a copy — replaced by measuring it.
- The claim that rows are already zero-copy end to end — replaced per decision 1. No test or file is deleted.

## Corner cases forbidden vs handled
**Forbidden:** keeping writing space past the one call it was lent for; being given space
without being told how much; claiming space and never using it, or twice; committing part
of a row then failing; asking for space of no size; reporting more written than was lent;
disturbing the buffer while filling it. Each is impossible to express, or refused at the
door with nothing half-done.
**Handled (one):** a client that writes past the space it was told it has. It cannot be
forbidden — zero-copy means handing over real memory — and it is exactly the exposure the
existing "hand over bytes you already have" call has always carried, so it is no new class.

## Decisions for you   (2 — the first has no default on purpose)
1. **Do we now promise zero-copy across the *whole* send path — from where a client's code
   produces the row to where a subscriber reads it — or keep promising only the half inside
   Fletcher and publish the gap as a named limit?** (a) the seam **permits** an uncopied row
   end-to-end, for a client that uses the new call · (b) narrow + limit.
   **Recommendation:** (a) — you ruled zero-copy a requirement, not a trade, and the narrow
   promise is true but useless to the C#/Rust clients it exists to serve. (a) is worded as
   *permits*, not *guarantees*: a client that ignores the new call still copies, and the seam
   cannot stop it.
   **Default:** none — this edits frozen text, so unanswered the stage does not close.

2. **How much may a green guard claim?** (a) that the interface *permits* an uncopied send,
   measured with a stand-in client, said plainly · (b) that real C#/Rust clients send
   without copying.
   **Recommendation:** (a) — no real binding exists to measure yet, and (a) matches the four
   previous times you chose the narrow honest claim. **Default:** (a).

## Risks accepted / debt carried
- The two amended sentences are carried by review, not a machine: deleting them reddens nothing.
- Sized from reading; the two previous amendments landed at 3–4x estimate, so the band is 600–1100.
- All runs local on Windows; Linux stays unverified per your 2026-09-03 handoff ruling.
- If the refusal here should carry the distinct re-entrancy number you allocated on 2026-09-03, that belongs to a sibling amendment — this one will ask, not allocate.

## Numbers
Declared net lines: +640 / −25 · new public surface: 1 · design cycles used: 1/2

---
*As landed (2026-09-05, PM at close):*
**+1114/−37** vs declared +640/−25 — 1.3% over the design's 600–1100 band, ruled **earned**. Charter held; surface **1**.
**Design cycles 1/2 · fix cycles 1.** Unpredicted: `bool lending_`, deviating from "no flag, no state" because the design's
mechanism was **disproved** (ruled justified and minimal), and `status.hpp` as compliance F4's own fix.
Raised for PDA-ABI: **A1-DEBT-6** — the overflow guard sits at one door, not at the arithmetic.
