# D7O-3 — Stage Brief (2026-11-12)

**In one sentence:** a subscriber that asks for very large batches, up to the largest number it can
write, now gets its rows. Before, every row of a perfectly good topic was reported lost, for the
life of the subscription.
**Forcing test:** `MaxRowsLargerThanAnyWindowIsUsedAsGiven` asks for the largest possible batch size
and proves the rows arrive: on the timeout, or at unsubscribe when there is none. A third case
proves a batch size above the new up-front limit still fills to exactly the size asked for.
**Guard:** `EveryBatchOptionIsRefusedExactlyWhenOutOfRange` tries 42 combinations of nonsensical,
edge and huge settings. Each is refused exactly when it is nonsensical, and every accepted one
delivers its row.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batched subscribe, very large batch size | CHANGED: rows delivered, not all reported lost | Ruling "Fix in D7O"; L15 |
| Header and README | CHANGED: "any other value is used as given" holds again for both settings | Owed from D7O-2 |

## Deleted
- A behaviour, disclosed: a good topic judged unreadable because room for the batch could not be
  set aside up front.
- One duplicated block of code that set that room aside. No file, type or test is deleted.

## Corner cases forbidden vs handled
**Forbidden:** the batch size deciding whether a topic can be read, or how much memory is taken
when each batch starts; a quiet cap on the batch size itself (the third case fills exactly to the
size asked for); a second place that judges the settings.

**Handled:** if setting room aside still fails (the machine is short of memory), the failure is
ignored and the batch grows as rows arrive. A machine's failure cannot be forbidden from here.

## Decisions for you
None.

**Disclosed (decided by L10 and L15, not a choice):** room for a batch is set aside up front only
up to about 65,536 rows. Up to that size nothing changes. Above it, memory follows the rows actually
received, with a little copying while a big batch fills. Setting aside room for the full number, as
today, could crash the delivery thread for some legal sizes (L10 forbids this). The number decides
only when a batch closes (L15).

## Risks accepted / debt carried
- A batch that outgrows memory before it closes is lost and counted, as it is today (L1, L10).
  Note for a future round, not a decision: delivering such a batch early would change law L4.
- Reported, not fixed: a good topic is also judged unreadable for life if building its decoder runs
  out of memory. That is a different cause from this stage's, and is raised with the PM separately.
- A benchmark comment in the encoding domain describes the old sizing. It is not edited here.

## Numbers
Declared net lines: +16 / −10 (production; tests ~+190) · new public surface: 0 · design cycles: 1/2

---
*As landed (2026-11-12, `c18e95e` + fix `57aa4bb`):*
Production +35/−18 vs declared +16/−10 (comments mostly; code-only +14/−13 before the fix; >50% over, owner note at round close); tests +414. Fix cycles 1: a failed pre-size now resets the builders (4b S1, an L10 hazard the design missed); options `const`; header "lost whole" → "lost". Added beyond the design: d7 Q8 and Findings (d), BC-3 registered gap (decoder-construction allocation failure, open). Decisions 1–2 became disclosures before the owner read the brief.
