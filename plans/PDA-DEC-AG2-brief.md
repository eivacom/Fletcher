# PDA-DEC-AG2 — Stage Brief (2026-09-05, revised 2026-09-06 to your three answers)

**In one sentence:** the two things that cross the interface with no published shape — the side data
a message carries and the explanatory text of a refusal — get one, so another program reproduces
them exactly instead of guessing from how Fletcher happened to be built.
**Forcing test:** `AnAttachmentSetIsReconstructibleFromItsPublishedFormAlone` +
`ARefusalIsReconstructibleFromItsNumberAndMessageAlone` — a stand-in foreign client shown only
what Fletcher publishes rebuilds both exactly; live negative controls prove neither is inert.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| The side data a message carries | CHANGED | Gains a stated, repeatable order and one published way to walk it |
| A refusal's explanatory text | CHANGED | Now part of the refusal, and survives being handed to another language |

## Deleted
- The old free-form side-data container and every incidental way to poke at it — replaced by one
  published way to walk it, in one stated order. No test is deleted; bodies move to the new form.
- The published "a driver may be written in Rust or C#" licence — behaviour gone, disclosed: your 2026-09-05 ruling, not an oversight.

## Corner cases forbidden vs handled
**Forbidden:** side data coming out in a different order on a different machine; two ways to walk one
set; the same label twice; a refusal message silently cut short at a language boundary; a refusal
whose reason lives anywhere but the refusal; asking for the tenth item of a set of three.
**Handled:** no message-length limit is invented — none has ever been measured; harmless odd bytes
are left alone, because only a zero byte truncates.

## Decisions — all three answered 2026-09-06; nothing outstanding
1. **Same message, same bytes on the wire — DECIDED: yes, a fixed stated order.** Side data goes
   out in one order everywhere. Your word is the sole authorisation for moving those bytes, and it
   covers side-data ordering only; anything else that would move a byte comes back to you.
2. **A side-data label containing a zero byte — DECIDED: refused.** A label that "works" today by
   accident stops working, loudly, instead of silently overwriting another one abroad. Your word is
   the sole authorisation for that new rule, and it covers this refusal only.
3. **Listing what protocols are available — DECIDED: no, only Fletcher's sentence.** Nothing new is
   published for an application to call. The refusal text still names what is available and is still
   made repeatable, so an operator reads the same sentence on every machine; an application that
   wants a "did you mean…" list must read that sentence or keep its own.

**No new question this revision.** Re-checking the tree for decision 3 found the available-protocol
list is *already* built in a stable order, so the repeatability you asked for costs nothing new —
this stage adds a guard that keeps it that way.

## Risks accepted / debt carried
- Side-data order on the wire changes for multi-attachment messages — authorised by you, not assumed.
- The claim is scoped to *this* tree, measured with a stand-in client: no real C#/Rust binding exists yet.
- Used in ~15 files; if a public surface depends on its free-form shape, the stage stops and asks.

## Numbers
Declared net lines: +830 / −250 (band +650/+1300 adds) · new public surface: 1 · design cycles: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
