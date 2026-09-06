# PDA-DEC-AG2 — Stage Brief (2026-09-05, revised 2026-09-06 to your five answers)

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
- Two published documents describing the old shape are corrected in the same change, so nothing we publish contradicts the software.

## Corner cases forbidden vs handled
**Forbidden:** side data coming out in a different order on a different machine; two ways to walk one
set; the same label twice; a zero-byte label, whether attached locally **or arriving from another
machine** (an arriving one is dropped as malformed, like five malformations already rejected today);
a refusal message cut short at a language boundary; a refusal whose reason lives anywhere but the
refusal; asking for the tenth item of a set of three.
**Handled:** no message-length limit is invented — none has ever been measured; harmless odd bytes
are left alone, because only a zero byte truncates.

## Decisions — all five answered 2026-09-06; nothing outstanding
1. **Same message, same bytes on the wire — DECIDED: yes, a fixed stated order.** Side data goes
   out in one order everywhere. Your word is the sole authorisation for moving those bytes, and it
   covers side-data ordering only; anything else that would move a byte comes back to you.
2. **A side-data label containing a zero byte — DECIDED: refused.** A label that "works" today by
   accident stops working, loudly, instead of silently overwriting another one abroad.
3. **The same label arriving from another machine — DECIDED: refused there too.** The message is
   dropped as malformed by the checks that already drop malformed messages, quietly and safely.
4. **Listing what protocols are available — DECIDED: no, only Fletcher's sentence.** Nothing new is
   published for an application to call. The refusal text still names what is available and is still
   made repeatable, so an operator reads the same sentence on every machine.
5. **One kind of message text changes — DECIDED: amend the published wording to say so.** A refusal
   about a topic name containing a zero byte now shows that byte written out rather than cut off —
   the only message anyone sees differently, and now named in writing rather than left implied.

**No new question this revision:** everything else the review raised is implementation work.

## Risks accepted / debt carried
- Side-data order on the wire changes for multi-attachment messages — authorised by you, not assumed.
- The claim is scoped to *this* tree, measured with a stand-in client: no real C#/Rust binding exists yet.
- Real edits land in ~15 files (54 mention the container, most only in passing); if a public surface
  depends on its free-form shape, the stage stops and asks rather than adding a compatibility layer.

## Numbers
Declared net lines: +920 / −270 (band +700/+1600 adds) · new public surface: 1 · design cycles: 3/3

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
