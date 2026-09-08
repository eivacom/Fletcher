# PDA-DEC cycle-meter extraction

Source: `plans/PDA-decouple-progress-log.md` (1,454 lines, as of the latest entry —
PDA-DEC-AG2 close). Read-only extraction; every figure below is copied verbatim
from the log's own text, not computed or inferred, **except** the "rulings" column
where the log itself uses a `ledger X → Y` before/after-count convention (noted
per row) and the four column totals (arithmetic sums of the cells above them,
explicitly marked as floors wherever an input is missing or is itself a floor).

Caps quoted in the log: design cycles capped at **2**; implementer-launch tripwire
at **5**. Neither is universal — several items report launches without a `/5`
denominator at all (design cycles are consistently given a cap where stated).

## Table

| Item | Design cycles | Fix cycles | Implementer launches | Owner touches | Rulings (ledger) | Notes |
|---|---|---|---|---|---|---|
| PDA-DEC-1 | 2/2 | 2 | 3/5 | 3 | — not cited by number | |
| PDA-DEC-2 | 1/2 | 1 | 2/5 | 1 | 3 (entries 20–22) | first-cycle design close |
| PDA-DEC-3 | 2/2 | 2 | 3/5 | 6 | 3 (ledger 23–25) | owner touches (6) exceeds the 3 named ledger entries — the log records more owner interactions than formal ledger rulings here; not a contradiction, just an observed asymmetry |
| PDA-DEC-4 | 1/2 | 2 | 5/5 | 3 | 3 (ledger 26–28) | first-cycle design close; launches hit the 5/5 tripwire exactly — "two launches wrote nothing… so three were productive — not a thrash signal" |
| PDA-DEC-5 | 1/2 | 2 | 3/5 | 0 | none ("Owner touches: none") | first-cycle design close |
| PDA-DEC-6 | 2/2 | 2 | 3/5 | 0 | none stated | |
| PDA-DEC-7 | 2/2 | 2 | 3/5 | 0 | none stated | the ruling that spawned PDA-DEC-1H is credited to 1H, not here |
| PDA-DEC-1H | 0 (compressed) | 2 | 3/5 | 1 (the ruling that created the item) | 1, unnumbered | design "0" is a **real reading** (item ran compressed, no architect step), not a missing figure |
| PDA-DEC-8 | 2/2 | **1** (final) | **2/5** (final) | 1 | 1, unnumbered (the scope ruling) | **the item states two different cycle meters.** Mid-entry (before close): "design 2/2 · fix 0 · launches 1/5 · owner touches 1". After the close gate FAILED on the first run (missing Stage Brief delta) and PASSED on re-run: "design 2/2 · fix 1 · launches 2/5 · owner touches 1". Table uses the final, post-re-run figures; the earlier ones undercount fix cycles and launches by 1 each |
| PDA-DEC-9 | — | — | — | — | — | **no cycle-meter reading anywhere in this item's section** — no "design/fix/launches/touches" figures of any kind, only unrelated prose mentions of "ruling" (the 2026-09-01 and 2026-09-03 rulings it documents, not ones it consumed) |
| PDA-DEC-A4 | 2/2 (at cap) | 3 | 4 (no /5 given) | 5 | 5 (ledger 34 → 39) | "ledger 34 → 39" is a before→after count, not an inclusive range: 39−34 = 5, matching the stated "5 rulings" |
| PDA-DEC-A1 | 1/2 | 1 | 2 (no /5 given) | — not stated | not stated as a count — rulings 44 and 45 individually named in prose, but no tally or ledger-delta given | first-cycle design close; owner-touches figure is simply absent from this item's numbers paragraph |
| PDA-DEC-AG1 | 2/2 | 3 | **≥4 (floor)** | — not stated | not stated as a count — rulings 48 (superseded) and 52 individually named, no tally | "one launch was interrupted and is absent from the PM's dispatch record, so that figure is a floor" — stated explicitly as a floor, kept as a floor here |
| PDA-DEC-AG2 | 2/2 | 2 | 3 (no /5 given) | — not stated | 6 ("Six owner rulings, 53–58") | rulings given as an explicit count **and** an inclusive range that agree (58−53+1 = 6); no separate "owner touches" figure given |
| PDA-DEC-A5 | 2/2 (at cap) | 2 | 4 (no /5 given) | 2 | 4 (ledger 39 → 43) | **the item appears under two separate `## PDA-DEC-A5` H2 headers** in the log — an initial write-up (no cycle meter at all) followed by `### … fix cycle 1` / `### … fix cycle 2` subsections, then a second, differently-titled `## PDA-DEC-A5` section that carries the only cycle meter, the final Numbers line, and the ledger range. Table uses that final section. "ledger 39 → 43" again reads as a before→after count: 43−39 = 4, matching |
| **Totals** | **22** (floor — PDA-DEC-9 missing) | **27** (floor — PDA-DEC-9 missing) | **41** (floor — PDA-DEC-9 missing *and* AG1 is itself a floor of ≥4) | **22** (floor — PDA-DEC-9, A1, AG1, AG2 all missing) | 24 explicitly summed via per-item ledger ranges (2+3+4+A4+A5+AG2) | see reconciliation below |

## Ledger reconciliation against the round's 59 entries

Per-item ranges/deltas actually stated in the log, in ledger order:

- PDA-DEC-2: entries **20–22** (3)
- PDA-DEC-3: ledger **23–25** (3)
- PDA-DEC-4: ledger **26–28** (3)
- PDA-DEC-A4: ledger **34 → 39** (+5)
- PDA-DEC-A5: ledger **39 → 42** (+3, initial) then ruling **43** in fix cycle 1 (+1) — final line states **ledger 39 → 43 (4 rulings)**
- PDA-DEC-AG2: **53–58** (6, individually named)

Items 2→3→4 chain contiguously (20‑22, 23‑25, 26‑28 = 20–28 with no gap), and A4→A5
chain contiguously onward (34→39, 39→43), so **20–43 is fully and consistently
accounted for by items 2, 3, 4, A4, A5 — 24 entries, matching the arithmetic sum
of their stated counts exactly** (43 − 20 + 1 = 24).

That leaves **35 of the 59 entries unaccounted for by any explicit per-item
count in this log**:
- **Entries 1–19** (before item 2's range starts): not attributed to any item by
  number anywhere in this log — PDA-DEC-1's "owner touches 3" is the only
  candidate window, but the log never cites a ledger number for it.
- **Entries 29–34** (between item 4's "…28" and A4's "34 →…"), 6 entries: only 2
  are even generically claimed, by PDA-DEC-1H ("owner touches 1, the ruling
  that created the item") and PDA-DEC-8 ("owner touches 1, the scope ruling") —
  neither cites its own ledger number, and PDA-DEC-5/6/7/9 in the same window
  report 0 or no owner touches at all. 4 of the 6 have no claimant in this log.
- **Entries 44–59** (after A5's "…43"), 16 entries: individually named without a
  formal per-item tally are 44, 45 (PDA-DEC-A1), 48 and 52 (PDA-DEC-AG1) — 4 of
  16. The remaining 12, including entry **59** itself, are not named anywhere in
  this log (PDA-DEC-AG2's own explicit 53–58 is already counted in the 24, not
  in this remainder — recompute: 44–59 is 16 entries, of which 53–58 (6) belong
  to AG2 and are already in the 24-sum; so this bucket's *truly unaccounted*
  slice is 44–52 minus {44,45,48,52} = 46, 47, 49, 50, 51, i.e. **5 entries with
  no citation in this log at all**, plus entry 59 (1 more) = 6 fully uncited in
  this trailing window).

**Bottom line: the log's own per-item numbers reconcile a contiguous, internally
consistent 24 of the ledger's 59 entries (20–43, items 2/3/4/A4/A5). The other
35 are not reconciled by any stated per-item figure** — 19 of them (1–19) have
no item ever cited against them, 4 of the 29–34 gap have no numbered claimant
even though two items claim a generic 1-ruling touch each, and roughly 10 of the
44–59 tail are similarly uncited (4 are individually named without being
summed: 44, 45, 48, 52).

## Answers to the five questions

1. **Design cycles: 22 (floor).** Fix cycles: 27 (floor). Implementer launches:
   41 (floor). Owner touches: 22 (floor). All four are floors because
   PDA-DEC-9 supplies no figures at all, and the launches total is additionally
   a floor because PDA-DEC-AG1's own "≥4" is a stated floor, not an exact count.
2. **1 item lacks a meter reading entirely: PDA-DEC-9.**
3. **4 items closed on the first design cycle** (design cycles = 1/2, cap 2):
   PDA-DEC-2, PDA-DEC-4, PDA-DEC-5, PDA-DEC-A1.
4. Ledger ranges recorded: DEC-2 (20–22), DEC-3 (23–25), DEC-4 (26–28), DEC-A4
   (34→39), DEC-A5 (39→43, folding in fix-cycle-1's ruling 43), DEC-AG2
   (53–58). These chain to a contiguous, self-consistent 24 entries (20–43).
   Against the round's stated 59-entry ledger total, **that leaves a 35-entry
   gap** the log's per-item figures do not reconcile — 19 before entry 20
   (nothing in this log ties them to an item), ~4 in a 29–34 window between
   items 4 and A4 (two items claim a generic single touch each, but neither
   cites its ledger number), and ~10 in the 44–59 tail after A5 (only 44, 45,
   48, 52 are individually named without a per-item sum; entry 59 itself is
   never mentioned in this log).
5. Internally inconsistent/notable points: **(a)** PDA-DEC-8 states two
   different cycle meters for itself in the same section (fix 0→1, launches
   1/5→2/5) after its close gate failed once and was re-run — the earlier
   figures are stale, not wrong for their moment, but both remain on the page.
   **(b)** PDA-DEC-A5 is written under two separate `## PDA-DEC-A5` H2 headers
   with different titles; only the second carries any cycle meter. **(c)** the
   `ledger X → Y` notation means an inclusive range in items 2/3/4 (literal
   entries) but a before→after delta in A4/A5 (39→42 then 39→43, i.e. the
   count grew, not "entries 39 through 43 happened twice") — both are
   self-consistent once decoded, but the same arrow phrasing carries two
   different meanings across the log. **(d)** owner touches is the meter most
   often dropped altogether — unstated for the round's last three items in
   sequence (A1, AG1, AG2) plus PDA-DEC-9, i.e. 4 of the last 6 items in the
   log carry no owner-touches figure at all, even though three of those four
   do name individual ruling numbers in prose.
