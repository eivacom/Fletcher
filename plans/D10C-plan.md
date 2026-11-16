# D10C — the C# switch round — Plan and tracker

Written by `round-designer` on 2026-11-15 from [the test ladder](D10C-test-ladder.md). It has one stage per rung, except stage 9, which pairs rungs 9 and 10. Domain: [10, the C# binding](../docs/domains/d10-csharp-binding.md), created by this round and owner-confirmed on 2026-11-15. Branch: `feature/csharp-switch`, cut from `feature/shim-server-variant` (D9S, not yet on `main`).

**Purpose (owner-confirmed):** A C# app gets its batches from Fletcher's one batcher, the C++ one, and C#'s own copy is gone, while an app that does not need batches still runs on the small, Arrow-free shim.

Status: ⚪ not started · 🔴 in progress · 🟢 done. Lane: **L** marks a light-lane candidate by size (60 touched lines or fewer, in 3 files or fewer), and **F** marks the full loop. Each subject is the ladder's rung sentence, cited here and not restated; the ladder's rows give the external path, laws, BC rows and test dispositions.

| Item | Subject (ladder rung) | Rung | Inherits | Est. lines / files | Lane | Status |
|---|---|---|---|---|---|---|
| D10C-1 | Domain 7's own suite proves the four behaviours only C#'s batcher tested | 1 | d10 Q9 port list (audit part B); ruling :595 (test-only exception; the big-row case runs as a heavy test) | 0 prod (tests only) | L | 🟢 |
| D10C-2 | Every number the C surface hands out has the same value in C# (`Handover = 4`, `BatchReason` 1-3) | 2 | d10 L2, D34, Q11 | 15–30 / 2–3 | L | 🟢 |
| D10C-3 | Per-row `Subscribe` runs on the edge shim with no batcher; an undecodable row arrives as a null batch with `RowsDropped = 1` | 3 | BC-16 (DEFINE); :536 (1), :575 (3), (5); BC-2 note | 60–110 / 1–2 | F | 🟢 |
| D10C-4 | The loader loads the server shim when present, fails loudly if it won't load, never both; CI gains a server leg and the renamed copy retires | 4 | BC-10 (EXTEND-AND-NARROW); :536 (2), :575 (1); retires :525's staging | 80–155 / 3–4 | F | 🟢 |
| D10C-5 | A server package carries exactly the server shim, depends on the base package, and the package rules check it | 5 | BC-17 (DEFINE); d10 L15, Q7 default; audit F (Interop-only rule) | 110–200 / 5–7 (split 5a/5b if over) | F | 🟢 |
| D10C-6 | `SubscriberArrow` refuses a row and a batched subscription to the same topic | 6 | :575 (6); d10 L3 ruled exception | 25–50 / 1 | L? | ⚪ |
| D10C-7 | The switch: batches come from domain 7 on the server shim; the edge shim gives the named NotSupported | 7 | BC-12/13 extensions, BC-8, BC-9; :551 (1)–(3), :575 (2), (5); D24 Arrow-import risk (stop if false) | +195–255 / −25, 4–5 files (split 7a/7b if over) | F | ⚪ |
| D10C-8 | C#'s own batcher is gone from the packed assemblies | 8 | d10 L4; audit MUST-DELETE (6 tests); BC-9 narrowing | −470 to −520 / 3 | F | ⚪ |
| D10C-9 | Dispose from inside a batch handler is served; bad batch options pass through as `FletcherException(InvalidArgument)` | 9, 10 | d10 L11; :536 (3), :575 (4); BC-8 grid, BC-14; `MaxTimeout` removed | 5–40 / 1–3 | L? | ⚪ |
| D10C-10 | A failed hand-over reaches the C# handler with the failure in `BatchStatus` | 11 | d10 L9, D26; :551 (3); d9 test-only fault shim | 40–75 / 3–4 | F | ⚪ |
| D10C-11 | The purpose end to end from packages: the server package gives batches; the base package alone runs on edge and gets the named NotSupported | 12 | the round purpose; d10 L5–L7 | 10–40 / 1–3 | L | ⚪ |

## Round exit

- All 11 items 🟢, with d10's owed laws marked `[built]` and their tests named.
- The managed batcher is deleted, and the 6 MUST-DELETE tests are gone.
- The ship gate (ruling 2026-11-12) is lifted only at round close.
