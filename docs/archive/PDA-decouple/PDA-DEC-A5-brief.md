# PDA-DEC-A5 — Stage Brief (2026-09-04, cycle 2)

**In one sentence:** two different topic names can no longer become one topic on the wire —
the one case in the tree where Fletcher gives a silently wrong answer.

**Forcing test:** `Segments.SegmentsThatAliasOrTruncateAreRefused`, plus
`TopicNames.AmbiguousSegmentsAreRefused` against real Fast DDS and XRCE — proves a name shape that
would collide with a different topic is rejected at once, on all three protocols, on all four operations.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Topic naming rules + spec §3.5 | CHANGED | Five name shapes are rejected instead of accepted, and protocols no longer choose their own way to build a name from its parts; every name still accepted behaves exactly as today, and rejection reuses the existing "invalid argument" code, so no new error number |

## Deleted
Nothing is retired. One sentence of the frozen interface specification is deleted and replaced; no
file, setting or test goes away, because this tightens a check that already exists in exactly one place.

## Corner cases forbidden vs handled
**Forbidden** (refused at the door, no recovery, no partial mode): a name part containing a zero byte (today
chopped off on one protocol); a part containing the separator (today makes "a/b" and "a"+"b" the same topic on
all three); an empty part; a part starting with `__`, where the two DDS protocols keep their own hidden channels.
Also impossible by construction: two protocols disagreeing on what a name means, and any hidden tidying-up such as case-folding.

**Handled**, with why it could not be forbidden: a name a particular transport rejects for its own reasons
— limits differ per transport, refusing the union at Fletcher's level would reject names that work, and it already fails loudly.

## Decisions for you   (2–4 max)
1. **A topic part that itself contains a "/" — reject it, or keep it working?** Today `"a/b"` as one
   part and `"a"`+`"b"` as two parts are the *same* topic, so one can silently receive the other's data.
   Options: (a) reject outright · (b) keep both working by rewriting the name on the wire · (c) declare
   the collision intentional and document it. **Recommendation:** (a) — a loud refusal beats a silent wrong
   delivery; (b) changes wire bytes for names that work today, which is a separate stop-and-ask; and no remote
   client loses a working topic, because Fletcher's own gateway can never produce such a part.
   **Default if you don't answer:** (a).
2. **An empty topic part — reject it too?** `{"a", ""}` and `{""}` are accepted today and name something
   degenerate; unlike case 1 they are not a collision. Options: (a) reject, matching the existing rule
   that an empty topic names nothing · (b) keep accepting them. **Recommendation:** (a) — one rule at
   one level is what a language binding can reproduce, and neither Fletcher's own gateway nor any code
   in the tree uses one. **Default if you don't answer:** (a).
3. **Both DDS protocols keep a hidden companion channel named `__schema` beside each topic, and a part
   named `__schema` lands on it. Reserve the `__` prefix?** Options: (a) reject any part starting with
   `__` · (b) leave the collision open · (c) allow only letters, digits, `_`, `-`. **Recommendation:**
   (a) — closes the whole reserved namespace, where (c) rejects dots and spaces that work today.
   **No default — needs your explicit word:** it adds a refusal 2026-09-03 did not name.
   *Background (skippable): unlike 1 and 2, a WebSocket client can send such a part today.*

## Risks accepted / debt carried
- Names using the rejected shapes stop working, loudly, with no migration path — deliberate, and the reason decisions 1-3 are yours.
- Evidence is local Windows runs only; a Linux-only difference here is a question for you, not a local fix (2026-09-03). No compatibility path is created.
- One protocol's `__schema` behaviour was not establishable from the code, so decision 3 removes the question rather than answering it.

## Numbers — *as landed, 2026-09-04, PM at close*
Declared +535/−15 · **landed +1055/−35** (2.0×) · surface **0** · **2 design cycles (cap) · 2 fix
cycles · 4 launches · 2 owner touches · 4 rulings.** **Five refusals shipped, not four:**
review found two accepted names agreeing on 255 bytes were one topic on Fast DDS (5 of 5 rows to the
wrong subscriber), so you ruled a **246-byte** bound. Also unpredicted, **disclosed rather than
implied: the gateway's accepted set narrowed** — it silently repaired `"a//b"` into `"a/b"`; that
repair is gone, so such a name now returns an error frame. Rulings 40/41 were priced on the gateway
never producing such a part — true *because* of that repair.
