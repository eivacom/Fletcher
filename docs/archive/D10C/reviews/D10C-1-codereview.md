# D10C-1 code review (light lane, step 4b)

Base `6b3f5c7`, working tree: two test files, +277/-0, and an untracked `plans/D10C-evidence.md`. Proof of read:
`plans/RIX-rulings.md` is 603 lines with 45 `## ` entries; the last is `## 2026-11-15 — D10C ladder: the domain-7 port tests` (:595).
Also read: the brief, the full diff, `BatchSink`/`NoTriggerBatchOptions` (:492, :707), the existing heavy tests,
`tests/CMakeLists.txt:42-65`, and `types.hpp:205` (`Set`).

**Verdict: blocking 0 · should-fix 1 · nit 3.**

## Conformance
It matches. Cases 1-3 and the 4/4c pair are the brief's table: same tests, files and clauses. All go through
`PublisherArrow`/`PublishDirect` and the batched `Subscribe`; `BatchDecoder` appears only in control 4c, as the allowed
premise check. Ruling :595 holds: only the two test files change, there is no d7 hook, case 4 runs at real size in the
`heavy` executable, and `CMakeLists.txt` is unchanged (0.39 s against the 60 s cap). Nothing was due for deletion; none happened.

## Findings
**S1 (should-fix, high confidence): case 3's `calls` died before the subscriber that captures it.** It was declared after
`sub` and captured by reference. On a red run (the `WaitFor(3, …)` assert fails), the test returns before `Unsubscribe`,
and `~SubscriberArrow`'s closing batch then does `++calls` on a dead object. Only a failing run reaches it, so not blocking.
Fix: declare `calls` beside `sink`.

**Vacuity: none of the five can pass vacuously.** Case 1: trailing bytes tolerated gives `num_rows` 2; a half accepted
breaks `rows_dropped` 2; index 0 is pinned to "good", size 1. Case 2: a lost empty key fails `size()==2`. Case 3: deliveries
2 and 3 must survive the throws, in `x` order. Case 4: no budget close fails the 20 s wait before `Unsubscribe`; B dropped or
split fails `recs[1]`; control 4c rules out "refused as malformed".

**Flakiness:** none; every read follows a bounded `WaitFor` or an outside `Unsubscribe` (synchronous by L9).
**Heavy peak:** the callback keeps facts only and resets `batch`; 648 MiB working set (579-835 MiB private), under ~1 GiB.
**Lines (+277 against ~190):** mostly assertion density and the two banners. Nothing must go; N1 is the one real saving.

## Nits
- N1: `HalfBudgetRowSize`/`WriteHalfBudgetRow` duplicated `OverBudgetRowSize`/`WriteOverBudgetRow` except for the count.
- N2: case 1's `Find("head")`/`Find("tail")==nullptr` are implied by `size()==1` plus `Find("good")`.
- N3: case 2 does not assert `reason == kRowLimit` (the brief does not require it).

## Fix cycle 1
S1 and N1 are fixed, checked in the tree: `calls` sits beside `sink` (:3597); the heavy file has one `RowSize`/`WriteRow(count)`.
