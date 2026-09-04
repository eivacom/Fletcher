# PDA-DEC-A5 — Stage Brief (2026-09-04)

**In one sentence:** two different topic names can no longer become one topic on
the wire — the one case in the tree where Fletcher gives a silently wrong answer.

**Forcing test:** `Segments.SegmentsThatAliasOrTruncateAreRefused` (plus
`ProviderConformance.AmbiguousTopicSegmentsAreRefused` per protocol) — proves that a name
shape that would collide with a different topic is rejected straight away, on all three
protocols and on all four operations.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Topic naming rules | CHANGED | Three name shapes are rejected instead of accepted; every name still accepted behaves exactly as today |
| Interface spec §3.5 | CHANGED | One sentence removed: protocols no longer choose their own way to build a name from its parts. Rejection reuses the existing "invalid argument" code — no new error number |

## Deleted
Nothing is retired. One sentence of the frozen interface specification is deleted and replaced;
no file, setting or test goes away, because this item tightens a check that already exists in exactly one place.

## Corner cases forbidden vs handled
**Forbidden** (refused at the door, no recovery, no partial mode): a name part containing a zero
byte (today chopped off on one protocol); a name part containing the separator (today makes
"a/b" and "a"+"b" the same topic on all three); an empty name part. Also structurally impossible:
two protocols disagreeing on what a name means, and any hidden tidying-up such as case-folding.

**Handled**, with why it could not be forbidden: a name a particular transport rejects for its own
reasons (limits differ per transport; refusing the union at Fletcher's level would reject names
that work — it already fails loudly).

## Decisions for you   (2–4 max)
1. **A topic part that itself contains a "/" — reject it, or keep it working?** Today `"a/b"` as
   one part and `"a"`+`"b"` as two parts are the *same* topic, so one can silently receive the
   other's data. Options: (a) reject it outright · (b) keep both working by rewriting the name on
   the wire · (c) declare the collision intentional and document it.
   **Recommendation:** (a) — a loud refusal beats a silent wrong delivery, and (b) changes wire
   bytes for names that work today, a separate stop-and-ask. **Default if you don't answer:** (a).
   *Background (skippable): today all three providers join parts with a bare `/` and escape nothing. Fletcher's own WebSocket gateway can never produce such a part, so no remote client loses a working topic under (a).*
2. **An empty topic part — reject it too?** `{"a", ""}` and `{""}` are accepted today and name
   something degenerate; unlike case 1 they are not a collision. Options: (a) reject, matching the
   existing rule that an empty topic names nothing · (b) keep accepting them.
   **Recommendation:** (a) — one rule at one level is what a language binding can reproduce; the
   gateway cannot produce these either, so nothing remote breaks. **Default if you don't answer:** (a).
3. **How far does the tightening go?** Options: (a) reject exactly the three shapes that cause a
   collision or truncation · (b) also restrict topic parts to a safe character set (letters,
   digits, `_`, `-`), as protocol selection already does.
   **Recommendation:** (a) — (b) would reject names with dots or spaces that work today and are
   not wrong; fix at the scope the guarantee is about. **Default if you don't answer:** (a).

## Risks accepted / debt carried
- Names using the three rejected shapes stop working, loudly, with no migration path — deliberate, and the reason decisions 1 and 2 are yours.
- Evidence will again be local Windows runs only; a Linux-only difference here is a question for you, not a local fix (your 2026-09-03 ruling).
- No new debt; no temporary compatibility path is created.

## Numbers
Declared net lines: +430 / -15 · new public surface: 0 · design cycles used: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
