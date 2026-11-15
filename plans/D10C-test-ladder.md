# D10C — test ladder

Round **D10C**, domain 10 (the C# binding), branch `feature/csharp-switch`, tree `7a9aa3e`. Purpose
(owner-confirmed verbatim, ledger :566): "A C# app gets its batches from Fletcher's one batcher, the C++
one, and C#'s own copy is gone, while an app that does not need batches still runs on the small,
Arrow-free shim." ("we should only have one and it should be the C++ one", ledger :203.)

Citations: `d10` = `docs/domains/d10-csharp-binding.md`, `d7`, `d9` likewise; `rul` = `plans/RIX-rulings.md`
(read in full: **593 lines**, highest entry `rul:575` "2026-11-15 — D10C pre-round audit: six collisions
and the BIND annotations"); `aud` = `plans/D10C-pre-round-audit.md` (T-rows = part C, census D-rows =
part E); `reg` = `docs/domains/bounded-context-register.md`; `SA` = `dotnet/src/Fletcher/SubscriberArrow.cs`.

Ceiling: **250 net production lines per stage**, tests excluded. Production = `dotnet/src/**`, csproj,
`scripts/`, CI YAML. Doc comments, READMEs and d10 marks are listed, not counted. Sizes are estimates,
given as added / removed where a rung deletes. Light lane: at most 60 touched lines and 3 files.

**Out of scope:** every `c-abi/` and `pubsub-arrow/` production file (d9, d7 read-only); domain 5's
`GatewayBatcher` and its tests (aud T50); generated C# (d1). The single exception is rung 1, test-only
in `pubsub-arrow/tests` (Finding 2). The D9S `NativeLoader.cs` version-line exception does not apply.

## Ground rules for every rung

- **Driven from outside, through a public C# interface:** `Eiva.Fletcher`'s public API (`ProviderRegistry`,
  `Publisher`, `SubscriberArrow`, `Subscriber`, the exception taxonomy), or a **test-host process** built
  on it and run from a staged folder (one shim per process, d9 L17), or the **packed `.nupkg` contents**
  with `scripts/check_*`, or the binding-abi-conformance lane. `NativeMethods`, `NativeLoader` and
  `SubscriberArrow`'s private types are never called by a rung test. Legs may call them; no leg is the
  only proof of a rung. Rung 1 is the one declared exception: it is driven through d7's public C++ API.
- **Publishing:** `Publisher` on the `inprocess` provider, through the same shim, as the suite does today.
- **Two legs from rung 4 on.** The dotnet lane runs the suite with the edge shim alone (edge leg) and with
  both shims staged (server leg). On the edge leg, every batched subject is asserted as C#'s BC-10 truth
  table: `SubscribeBatched` throws `FletcherException` (`NotSupported`, origin `Seam`) whose message
  starts with the shim's text verbatim and adds exactly one sentence naming the server package (D29);
  the handler is never called, and a later publish delivers nothing.
- **Timing assertions are one-sided** (as D7O, D9S): "nothing delivered" over a fixed window, then "exactly
  X at closing"; each such arm has a control (a 10 ms timeout gives `Timeout` within 2 s).
- **Ship gate (rul:278, D4).** No rung merges to `main`; the round lands whole. Nothing ships mid-round,
  which is what makes the declared red windows below acceptable.
- **Names and shapes are the architect's** (the server package id, a public variant property if any,
  the per-row `BatchReason`, the `BatchStatus` failure shape). This ladder names none of them.

## The ladder

| # | Subject (what becomes TRUE) | External path | Internal observation | Builds on | New work (est. net prod. lines / files) | Laws, BC rows; tests deleted / re-anchored |
|---|---|---|---|---|---|---|
| **1** | **Domain 7's own suite proves the four behaviours only C#'s batcher tested** (d10 Q9), so deleting C#'s copy loses nothing: rows split across two messages are dropped, not misaligned; an empty attachment key arrives in its window; a callback that throws does not stop the next batch; a row that would pass the offset budget closes the window first and lands in the next. | `pubsub-arrow`'s public `SubscriberArrow::Subscribe` (batched), from `pubsub-arrow/tests`. Case 4 per Q-A. | None. | Nothing: d7 as built. Placed first so a red port (a d7 defect) stops the round before any C# work leans on it. | **0 production lines.** Test-only, 1-2 test files (+ the `heavy` executable for case 4 under Q-A (a)). Light lane. **Expected green on arrival; declared guards.** A red case is a d7 finding: STOP, owner (fixing it is d7 production). Touches no d7 law: each case asserts d7 L1/L2 (case 1), L3 (2), L10/L12 (3), L4/L2 (4) as written. d7's doc Tests lines are not edited (Finding 2). | d10 L4 (precondition), D2; d7 L1-L4, L10, L12; aud Q9 list. Enables deleting T38 (rung 7) |
| **2** | **Every number the C surface hands out has the same value in C#**: `FletcherStatus` 0-10, `FletcherOrigin` 0-4 with `Handover = 4`, `BatchReason` 1-3 (`RowLimit = 1`, a visible change, D34), and the variant values. | The public enums, compared by name and number with `binding.h`'s published `enum`s and `#define`s (the header is the oracle, read by the test). | None. Leg: `Errors.ThrowIfFailed` maps origin 4 to `Handover`, not to an unnamed value (`Errors.cs:65-67`); unreachable from outside until rung 11. | Nothing. Rung 7 casts `fl_batch.reason` and rung 11 reads origin 4 through what this rung creates. | `Handover = 4`; `BatchReason` renumbered; the variant mirrored (enum + `fl_binding_variant` import, used by rungs 4 and 7). **~15-30 lines, 2-3 files.** Light lane. Docs: `FE:38`, `FE:57-75` (F17). Red: the table test fails on origin 4 and on all three reasons. | **L2** (both violations); D26 (origin half), D34; Q11; census D6. Keeps T44 (adds the table test) |
| **3** | **`SubscriberArrow.Subscribe` delivers each row as a one-row batch on the edge shim with no batcher behind it, and a row it cannot decode arrives as a null batch with `RowsDropped = 1`, never skipped.** Attachments, including an empty key, arrive with their row; a dictionary column arrives as its value type (D32); disposing the `SubscriberArrow` inside a row delivery is refused before anything is torn down. | `SubscriberArrow.Subscribe` on the dotnet lane (edge shim, as today). BC-16's conservation: good, malformed and schema-less rows (subscribe before declare on `inprocess`); rows offered = rows delivered + Σ `RowsDropped`, one row or one drop per handler call. | No managed `Batcher` is created for a per-row subscription (observed, e.g. through the batcher dictionary's count in a leg). | Rung 2 (`BatchReason` as renumbered). Reuses `Subscriber.Subscribe` (`fl_subscriber_subscribe`) and `FletcherCodec.Decode` (`fl_decode_rows`) as they are. | One per-row handler with one classify-and-count site (BC-16); **the one attachment-copy function** (census D16), created outside `Batcher` (whose own copy is not edited, D3, and dies at rung 8); `Subscribe` stops calling `SubscribeBatched`. **~60-110 lines, 1-2 files.** Docs: `SA:201-207`. Red: BC-16 property (today's per-row is the batcher's `MaxRows = 1`, a different path; a row-delivery refusal with no test is F3). The refusal arm is a guard (green both ways), declared. | **L7**, **L9** (per-row), **L11** (row half); D20, D30, D32; **BC-16 (DEFINE)**; census D8, D16. Re-anchors **12**: T1-T5, T8-T11, T29, T32 and T41 (per-row halves) |
| **4** | **A C# app loads the server shim when it is present beside it, otherwise the edge shim, never both; a present server shim that will not load stops the app with an exception naming the file and the OS's reason, with no fallback.** Holds in a loose layout and in the NuGet `runtimes/<rid>/native/` layout (F14: the runtime's default probing route too). | A test-host process on `Eiva.Fletcher`, run from staged folders: {edge}, {edge, server} in both layouts, {edge, unloadable server}. Assert it runs (publish and per-row subscribe), and the refusal's type and message. The dotnet lane gains the server leg (both shims staged; whole suite). The conformance lane stages the server file under its own name (T47; rul:525's renamed copy retires). | Exactly one `fletcher-c-abi*` module in the process; `fl_binding_variant` answers `SERVER` / `EDGE`. **Observation only:** the preference's external proof is rung 7 (batches only when the server shim is present), unless the design exposes the variant publicly (d10 L6 allows it), which makes it external here. | Rung 2 (variant import). Rung 3: per-row works on both legs, so the server leg is green at once. | The one resolver (`NativeLoader`), server candidates first in both routes, the loud failure. CI: the dotnet lane builds/locates the server shim and runs two legs; the conformance lane stages it by its own name. **~80-155 lines, 3-4 files** (`NativeLoader.cs`, `ci.dotnet.yml`, the conformance workflow). Docs: `NL:6-14, 58-87` (F14), `README.md:147-158` (F15). Red: the server shim present is never loaded (`NL:73`). Control: an unloadable server shim must not leave the edge shim loaded. | **L5**; D21, D28; **BC-10 (EXTEND-AND-NARROW)**: scan finds no `NativeLibrary.Load*`/`TryLoad` outside `NativeLoader`; BC-15; census D1, D17, D18. Re-anchors T42, T47. Keeps T43 (adds preference and no-fallback rows); L1 handshake rows run on both legs |
| **5** | **A server package exists that carries exactly the server shim for `win-x64` and `linux-x64` under its own file name and depends on the base package; the base package still carries only the edge shim within its 12 MB ceiling; no other package carries a native file.** | The packed `.nupkg`s, checked by `check_dotnet_packages.sh` over five packages, with three red controls (a `runtimes/` entry injected into `Eiva.Fletcher`; a GatewayClient dependency on the server package; a server package with anything but its two shims). `check_shim_dependencies.sh` on the server shim with Q7's default lists and floors. | None. | Rung 4: the package's layout is one rung 4 already loads from. | The server package project; pack in `cd.dotnet.yml` (server shim for both RIDs); BC-17's one rule list; Q7's per-shim limits (ceiling from the first measured pack + D-BIND-157's margin). **~110-200 lines, 5-7 files.** If the architect's estimate passes 250: **5a** package + BC-17 rules, **5b** Q7 limits. Docs: `Fletcher.Interop.csproj:6-10`, READMEs (F15, F12, F13). Red: no server package; the controls. A Q7 breach (a DLL or floor the server shim needs) is an owner stop by D-BIND-148's own terms. | **L15**; D10, D21; Q7; **BC-17 (DEFINE)**; census D9, D10. Re-anchors T48. Extends T49 |
| **6** | **A `SubscriberArrow` refuses a row and a batched subscription to the same topic with a named exception, whichever comes second; the first keeps delivering, and the other kind is accepted once the first is gone.** Different topics are unaffected. | `SubscriberArrow.Subscribe` / `SubscribeBatched`, both orders, on both legs (batched today is still the managed path, which works on edge). | None. | Rung 3 (per-row no longer is `SubscribeBatched`, so the two kinds are distinct calls). Lands **before** the switch so the silent same-topic replacement (d9 D33) is never reachable. | One topic map in `SubscriberArrow`, at its entry points, not in `Batcher` (D3 holds); it survives rung 7 unchanged. **~25-50 lines, 1 file.** Light lane if ≤ 60. Red: today both kinds are accepted. | **L3** (ruled exception), **L9**; D33 (rul:575 (6)) |
| **7** | **The switch. On the server shim a C# batched subscription receives each window domain 7 closes as a `RecordBatch` the handler owns and may keep after the callback, with domain 7's reason, `RowsDropped` and each row's attachments, one at a time, in window order, on one thread, and exactly one `Closing` per accepted subscription, empty when idle. On the edge shim it throws the named `NotSupported` (ground rules).** Late schemas, null-batch meanings, dictionaries (re-folded, D24), whole-window failures and in-handler `Unsubscribe` behave as d7 (V1-V4, V9-V12). A throwing handler is counted and raised, and the next batch arrives. Options cross as given (`MaxRows` int64; `Timeout` through one translation site; `InfiniteTimeSpan` = no timeout). | `SubscriberArrow.SubscribeBatched`, `Unsubscribe`, `Dispose` on both legs. C# replay of d7's `EveryCallbackRunsOnTheBatchThreadInWindowOrder`. A handler keeps a batch and reads it after later callbacks and after closing. | The shim's count of unmoved batches it released is 0 when C# moved (BC-13); `fl_binding_variant` called from one site in `dotnet/src`. | Rung 2 (`reason` cast 1:1), rung 3 (the attachment copy, reused: one function, two callers), rung 4 (server leg), rung 5 (the sentence names a package that exists), rung 6 (no same-topic collision). | Imports of `fl_subscriber_subscribe_batches`/`_unsubscribe_batches`, `fl_batch` (complete layout incl. `failure`, so rung 11 changes no signature), `fl_batch_fn`; one batch thunk (move, import, call, catch-count); `SubscribeBatched`/`Unsubscribe`/`Dispose` rewritten over the batched entry points (bit-63 ids); the edge sentence (D29); BC-14's one site lifted from `SchemaArrival.ToMilliseconds` (arrival unchanged; batch: negatives forwarded, never rounded to 0). **~+195-255 / −25 lines, 4-5 files.** **Near the ceiling:** if over, **7a** (all but attachments) then **7b** (attachments), with T17, T18 and the batched halves of T32/T41 a **declared red window** of one stage. Never split the edge refusal from the first batched call. Docs: `SA:4-94, 106-110, 147-157, 209-212, 252-260` (F16). Risk to confirm at design: Apache.Arrow's C Data importer must import a dictionary array; if it cannot, D24 is unmeetable without a C# conversion (L3): STOP, owner. | **L6**, **L8**, **L13**, **L14**, **L3** (D29), **L9** (V13), **L10** (thunk in reflection scope), **L12** (site); D13, D24, D25, D27, D29, D34; **BC-13 C# half (DEFINE)**; BC-14 site (EXTEND); census D2, D5, D11, D12 (L4 now unreachable), D13, D15. **Deletes 5:** T20, T30, T34, T38, T39. **Re-anchors 21:** T7, T12-T19, T21 (rewrite), T22-T28 (T25 rewrite), T33, T35, T36 (rewrite), T37, plus batched twins of T32, T41. Keeps T45 |
| **8** | **C#'s own batcher is gone: the packed `Eiva.Fletcher` assembly holds no window, row count, timer, deadline clock or batch thread for `SubscriberArrow`, and nothing reaches the framed decode.** | The packed `Eiva.Fletcher` / `.Interop` assemblies (metadata scan of the `.nupkg` contents: no `SubscriberArrow` type references `System.Threading.Timer` or `Stopwatch`; no `fl_decode_rows_framed` import). Plus L4's source scan of `dotnet/src/Fletcher` (BC-9's structural test). The whole suite stays green on both legs. | None. | Rungs 3 and 7: neither public path reaches `Batcher` any more, so this rung only deletes. | Delete `SA:353-827`, the internal ceiling constructor and `DefaultWindowByteCeiling`, `FletcherCodec.DecodeBatchFramed` and its import (d9 keeps the entry point, Q8 default). **~+0-10 / −480-520 lines, 3 files.** Net-negative, so inside the ceiling; a large review by line count, all deletion. Docs: BIND annotations already applied (rul:575 (7)); D19's list closed. Red: the scans find the batcher. | **L4**; D2, D3 (ends), D19; **BC-9 (EXTEND-AND-NARROW)**; census D12. **Deletes 1:** T40 (pins the deleted constant) |
| **9** | **Disposing a `SubscriberArrow` from inside its own batch handler is served: queued batches, then one `Closing`, then nothing, without deadlock.** | `SubscriberArrow.Dispose` from the handler, on the server leg (and V14's row-limit case). | None. | Rung 7 (a batch callback pushes no delivery frame, BC-6), rung 3 (the row-delivery refusal stays). | Normally none (d10 L11: "only a test"). **~0-15 lines, 0-1 file.** **Expected green on arrival; declared guard.** Red only if `Dispose` holds a lock across the native close. | **L11** (batch half), **L14** V14; d7 L9, D12; census D14; F3 |
| **10** | **A batch option out of range raises `FletcherException(InvalidArgument)` with domain 7's message, and nothing is delivered or left subscribed; anything in range is used as given:** `MaxRows` above `int.MaxValue`, a finite timeout past 49.7 days, `TimeSpan.MaxValue` (never early), `InfiniteTimeSpan` (no timeout), −1 tick (refused by domain 7, not 0 ms). `BatchOptions.MaxTimeout` no longer exists. | BC-8's 6 x 7 grid driven from C# on the server leg, message byte-equal to domain 7's; the BC-14 arms. A public-surface check that `MaxTimeout` is gone. Scans: no comparison or clamp on `MaxRows`/`Timeout` (BC-8); one `TimeSpan`-to-ms site in `dotnet/src/Fletcher` (BC-14). | None. | Rung 7 (forwarding and the site), rung 8 (the batcher's tick conversion is gone, so the BC-14 scan can be green). | Remove `BatchOptions.MaxTimeout` (visible). Any rounding fix the −1-tick arm finds. **~5-25 lines, 1-2 files.** Light lane. Mostly green on arrival; green arms declared as guards. Red: `MaxTimeout` exists; the scans before rung 8. | **L3** (D22, D31), **L12**, **L14** V5-V8; **BC-8 (EXTEND-AND-NARROW)**, **BC-14 (EXTEND-AND-NARROW)**; census D3, D4. Re-anchors T31 |
| **11** | **When the shim cannot hand over a window, the C# handler still gets it: a null batch, its rows in `RowsDropped`, its reason, and the failure (`Internal`, origin `Handover`, message) in `BatchStatus`; later windows arrive normally and rows offered = rows delivered + Σ `RowsDropped`.** | `SubscribeBatched` in a test-host process whose server file is d9's test-only hand-over fault shim (staged under the server name; test staging only), armed by a test-only P/Invoke of `fl_test_handover_fault_arm`, which only arranges the failure. | `fl_test_handover_fault_fired` reports the fault fired on window k. | Rungs 2 (origin 4), 4 (loader, staging), 7 (`fl_batch.failure` already mirrored; the null-batch path). | The thunk's failure branch (copy the borrowed message through `Errors`' one decoder; never `fl_error_dispose`); the public failure on `BatchStatus`. CI: build d9's tests (server variant) in the dotnet lane and stage the fault shim; no d9 edit. **~40-75 lines, 3-4 files.** Red: today the failure is invisible (`Handover` never reaches C#). | **L9**, **L8** (never dispose `failure`), **L2**, **L14** V11; D15, D26; **BC-12 C# half (DEFINE)**; census D7 |
| **12** | **The purpose, end to end from packages: an app referencing the server package gets batches from domain 7; an app referencing only the base package publishes, subscribes per row and decodes on the edge shim, has no server shim in its output, and gets the named `NotSupported` naming the server package from `SubscribeBatched`.** | `integration-tests/preview-bundle-consumer`, restored from the packed packages in two configurations (T46). The `native-batch` README snippet checked against it (D-BIND-142). | None. | Every earlier rung: rung 5's packages, rung 4's loader, rungs 3 and 7's paths. | Consumer project references and one CI step. **~10-40 lines, 1-3 files.** Light lane. Docs: `README.package.md:150-178, 225-229, 257-285`, `PREVIEW-TESTERS.md:10` (F15, F27). Red: the base-only consumer's batched call is not the named refusal (it succeeds or misses the sentence) until rungs 5 and 7. | **L6**, **L7**, **L15**; D9, D23, D25; purpose. Re-anchors T46 |

**Rungs: 12. Stages: 11.** Corpus: **6 deleted** (T20, T30, T34, T38, T39 at 7; T40 at 8), **38
re-anchored** (12 at 3; T42, T47 at 4; T48 at 5; 21 at 7; T31 at 10; T46 at 12), as aud part C lists.

| Stage | Rungs | Est. prod. lines (net) | Files | Light lane? |
|---|---|---|---|---|
| D10C-1 | 1 | 0 (test-only, d7) | 0 | yes |
| D10C-2 | 2 | 15-30 | 2-3 | yes |
| D10C-3 | 3 | 60-110 | 1-2 | no |
| D10C-4 | 4 | 80-155 | 3-4 | no |
| D10C-5 | 5 | 110-200 | 5-7 | no (split 5a/5b if over) |
| D10C-6 | 6 | 25-50 | 1 | candidate |
| D10C-7 | 7 | 170-230 (+195-255 / −25) | 4-5 | no (split 7a/7b if over) |
| D10C-8 | 8 | −470 to −520 | 3 | no |
| D10C-9 | 9, 10 | 5-40 | 1-3 | candidate |
| D10C-10 | 11 | 40-75 | 3-4 | no |
| D10C-11 | 12 | 10-40 | 1-3 | yes |

**No deliberate ceiling exception.** Two watch points with pre-declared splits: rung 7 (near 250 gross)
and rung 5. Stage 9 pairs two proof rungs that may arrive green, as D9S stage 5 did; each keeps its own test.

**Accumulation and deletion checks.** The port (1) precedes every deletion of a batcher test (7, 8). The
mirror (2) exists before the thunk casts reasons (7) and before origin 4 can arrive (11). The attachment
copy is created once (3) and reused (7); `Batcher`'s copy is never edited and dies at 8. The loader (4)
precedes every server-leg test; the server package (5) exists before the edge sentence names it (7). The
same-topic refusal (6) precedes the shared `fl_subscriber` (7), so no silent replacement is ever
reachable. The batcher becomes unreachable at 7 and is deleted at 8: **no coexistence bridge** (nothing
routes a build or an option to the managed path). `fl_batch` is mirrored in full at 7, so 11 adds a
branch, not a signature. Nothing built by a rung is deleted by a later one; the only declared red window
is 7a→7b, if that split is taken.

## Tests other than rungs

- **Guards (declared).** Rung 1's four ports (green on arrival). The L2 table (2) re-runs at every stage.
  The `NativeLibrary` scan (4), L4's scan (8), and the BC-8/BC-14 scans (10) re-run at every later stage.
  `ThunkDisciplineTests` covers the batch thunk (7). The edge-leg truth table runs for every batched
  subject from 7 on. Rung 3's row-delivery refusal and rung 9 are green both ways.
- **Controls.** 1: none needed beyond each case's own premise check (case 4: the row fits alone).
  4: an unloadable server shim must not leave the edge loaded. 5: the three package controls. 7: a 10 ms
  timeout gives `Timeout` within 2 s; a C#-side range check would change the message (byte equality at 10).
  11: unarmed, the fault shim delivers ordinary batches.

## Findings and owner questions

1. **Plan finding: the loader and the server package precede the switch.** The re-anchored batched tests
   need the server shim in the dotnet lane, and extending rul:525's renamed copy to that lane would build
   staging the loader then deletes. The edge sentence (D29) names the server package, so the package
   (5) must exist before the switch (7), not after it.
2. **Scope finding (owner, low): where the ports land.** d10 §8 rule 6 and Q9 say each case "is ported by
   a domain-7 round, or the owner rules it dropped"; the round's config puts them in this round, test-only.
   That is not a tier-1 conflict (rul:203 names no round). Recording the test names in d7's own doc would
   be a d7 edit; the ladder leaves d7's doc to its next round and records the ports in d10 §8. See Q-B.
3. **Case 4 cannot be ported by default-run test without d7 production code** (aud Q9 item 4: no
   test-only budget hook). See Q-A.
4. **No tier-1 disagreement found.** L1-L15, D1-D34 and rul:536/:551/:566/:575 agree with every rung.

**Q-A — How does domain 7 prove "a row that would overflow the window closes it first, and lands in the next"?**
- **(a) Port it as a `heavy` test in `pubsub-arrow/tests` (two ~128 MiB rows), test-only.** Users: no change;
  the case is proven only when the `heavy` label runs, like d7's existing budget tests. Architecture:
  no d7 production edit; d7 D11's heavy convention reused.
- **(b) Grant a named d7 production exception for a test-only budget hook**, so the case runs by default
  at a small size. Users: no change. Architecture: a d7 code change in a d10 round, needs its own
  review; d7's surface gains a test seam.
- **(c) Rule it dropped**: d7's heavy `OverBudgetRowAroundInCallbackUnsubscribeIsCounted` already
  shows the early close. Users: no change. Architecture: "lands in the next window" stays unproven
  in d7; rul:203's "ported over first" is met by ruling.
- **Recommendation (a); default (a).**

**Q-B — Do the four ports land in this round?**
- **(a) Yes, as a named test-only exception into `pubsub-arrow/tests`, d7's laws and doc untouched.** Users:
  the switch is not delayed. Architecture: d10 §8's "by a domain-7 round" is met in substance; d7's
  doc ingests the test names at its next round.
- **(b) A short domain-7 round first.** Users: the switch starts one round later. Architecture: d10 §8
  read literally; d7's doc updated in the same round as its tests.
- **Recommendation (a); default (a).**
