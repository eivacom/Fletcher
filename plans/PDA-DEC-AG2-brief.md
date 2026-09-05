# PDA-DEC-AG2 — Stage Brief (2026-09-05)

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
| "Which protocols are available?" | NEW *(decision 3)* | An application can ask, instead of reading it out of an error sentence |

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

## Decisions for you   (3)
1. **Should the same message always produce the same bytes on the wire?**
   Options: (a) yes — side data goes out in a fixed, stated order · (b) no — leave it to the build
   and publish "order is unspecified, sort it yourself".
   **Recommendation:** (a); (b) publishes the leak instead of fixing it, which you declined seven
   times. Cost: a message with two or more pieces of side data may go out in a different order than
   today's build chose — nothing breaks, every reader matches by label, not by position. **Default: (a).**
   *Background (skippable): today's on-wire order is `std::unordered_map` hash order, so two Fletcher builds can already emit different bytes for the identical message; locked decisions 11 and 13 make any wire-byte movement your call.*
2. **A side-data label containing a zero byte: refuse it, or accept it?**
   Options: (a) refuse it when it is attached · (b) accept it and publish the hazard.
   **Recommendation:** (a); under (b) two different labels silently become one at a C#/Rust boundary
   and one quietly overwrites the other — the silent-wrong-answer class you refused three times in
   September. Cost: a label that "works" today stops working.
   **Default: none — this adds a refusal to frozen text, so it needs your word.** Unanswered, it does not land.
3. **When an application asks for a protocol that isn't there, may it list what is?**
   Options: (a) yes — it can ask Fletcher for the available names · (b) no — it can only show
   Fletcher's sentence. **Recommendation:** (a) — a C#/Rust app can then offer "did you mean…"
   rather than re-printing English prose; the refusal text stays either way. **Default: (a).**
   *Background (skippable): one new query on the registry; the creation signature is untouched, so a driver path stays admissible for PDA-ABI.*

## Risks accepted / debt carried
- Decision 1 moves wire bytes for multi-attachment messages — raised to you rather than taken.
- The claim is scoped to *this* tree, measured with a stand-in client: no real C#/Rust binding exists yet.
- Used in ~15 files; if a public surface depends on its free-form shape, the stage stops and asks.

## Numbers
Declared net lines: +900 / −250 (band +700/+1300 adds) · new public surface: 2 · design cycles: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
