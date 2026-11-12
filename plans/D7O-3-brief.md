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
1. **Should room for a whole batch be set aside each time a batch starts, however large it is?**
   (a) No: room for at most about 65,000 rows, and a bigger batch grows as rows arrive. Users: up to
   65,536 rows nothing changes; above that, memory follows the rows received, with a little copying
   while a big batch fills. Architecture: follows L15, so the number only decides when a batch
   closes. · (b) Yes, as today, and a failure is ignored. Users: asking for 50 million rows takes
   gigabytes at the start of every batch, even one that closes on its timeout with 3 rows.
   Architecture: memory use is still dictated by the number asked for.
   **Recommendation:** (a), because the purpose is "get what you asked for". **Default:** (a).
2. **Asking for unlimited batches (huge size, no timeout) on a fast stream: when memory runs out,
   that batch is lost and counted. Acceptable for now?** (a) Yes, disclosed in the header. Users:
   any batch that outgrows memory is already lost this way today; the loss is reported, never
   silent. Architecture: holds L1 and changes no law. · (b) No: book a later round that delivers
   the batch early when memory runs short. Users: they get the rows. Architecture: a new reason
   for a batch to close, which changes law L4.
   **Recommendation:** (a), because the loss is visible and not new. **Default:** (a).
   *Background (both): the decoder is pre-sized from `max_rows` in `SetSchema` and `Close`.*

## Risks accepted / debt carried
- Reported, not fixed: a good topic is also judged unreadable for life if building its decoder runs
  out of memory. That is a different cause from this stage's, and is raised with the PM separately.
- A benchmark comment in the encoding domain describes the old sizing. It is not edited here.

## Numbers
Declared net lines: +16 / −10 (production; tests ~+190) · new public surface: 0 · design cycles: 1/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
