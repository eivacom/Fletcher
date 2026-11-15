# D10C — progress log

Session directive (2026-11-15): run from D10C-1 onward, item by item, until a stop-and-ask.

## D10C-1 — d7's suite proves the four behaviours only C#'s batcher tested (2026-11-15)

**Forcing test:** `SubscriberArrowBatchHeavyTest.ARowThatMeetsTheBudgetClosesTheWindowAndOpensTheNext` → 🟢 (guards, green on arrival)
**Lane:** light · **Brief:** `plans/D10C-1-brief.md` (no design doc)
**What landed:** four d7 port tests + one premise control, test files only (ruling :595): split rows dropped not misaligned (L1–L3), empty attachment key (L3), throwing callback does not stop the next batch (L10/L12), budget-meeting row opens the next window (L4, heavy, ~128 MiB rows).
**Interfaces:** none.
**Deleted:** none. 0a MUST-DELETE(1) = C# `ARowThatWouldPassTheCeilingFlushesTheWindowFirst` (T38); PM decision: deletion stays at rung 7 as scheduled, since :203 forbids deleting before its case is ported.
**Files touched:** `pubsub-arrow/tests/test_pubsub_arrow.cpp`, `pubsub-arrow/tests/test_pubsub_arrow_heavy.cpp`
**Reviews:** 0a MUST-DELETE(1) · 0b CLEAR · code-review blocking 0 / should-fix 1 / nit 3 (S1, N1 fixed) (`plans/reviews/D10C-1-*`)
**Verification:** pubsub-arrow 62/62, heavy 4/4 (0.92 s, peak ~648 MiB); full suite: c-abi edge 116/116, server 126/126; dotnet 1466/1466; binding-abi-conformance 11/11 ×2; pubsub-arrow-fastdds 4/4. **Not run:** `check_dotnet_packages.sh` (needs the Linux shim from the Docker devcontainer; Docker down). No packaging touched; owed from D10C-5.
**Close gate:** PASS (--light)
**Cycle meter:** design 1/2 · fix 1 · implementer launches 2/5 · stage words 2932
**Numbers:** declared prod +0/−0, tests ~+190 · actual prod +0/−0, tests +280/−11 · new public surface 0
**Commit / push:** `feature/csharp-switch` → origin
**Owner decisions this stage:** none
**Debt carried:** none. Accepted risk: no CI job runs `heavy` (:140, :595 (2)).
**Records corrected:** none

## D10C-2 — every C-surface number has the same value in C# (2026-11-15)

**Forcing test:** `AbiNumberTableTests.EveryNumberTheShimHandsOutHasTheSameValueInCSharp` (4 rows) → 🔴 (origin 4, all 3 reasons, variant) → 🟢; `fl_status` row a guard
**Lane:** light · **Brief:** `plans/D10C-2-brief.md` (no design doc)
**What landed:** C# names origin 4 (`FletcherOrigin.Handover`); `BatchReason` renumbered to the header's 1–3 (visible, D34); internal `FlVariant` + `fl_binding_variant` import for rungs 4/7. The table test reads `binding.h` from the repo and refuses entries it cannot parse. d10 L2 → `[built]`, Q11 closed.
**Interfaces:** NEW `FletcherOrigin.Handover`; CHANGED `BatchReason` values; internal `FlVariant`, `fl_binding_variant`.
**Deleted:** none.
**Files touched:** `NativeMethods.cs`, `FletcherException.cs`, `SubscriberArrow.cs`; tests `AbiNumberTableTests.cs` (new), `ErrorTests.cs`, `Fletcher.Tests.csproj`; `docs/domains/d10-csharp-binding.md`
**Reviews:** 0a CLEAR · 0b CLEAR (note: while `FlVariant` is internal the row is the sole proof of its numbers; brief corrected) · code-review blocking 0 / should-fix 2 / nit 3 (S1, S2, N1 fixed)
**Verification:** dotnet 1478/1478 (net8+net10, edge + probe shims); binding-abi-conformance 11/11 ×2; pubsub-arrow-fastdds 4/4; no dotnet integration lane touches the changed enums. **Not run:** `check_dotnet_packages.sh` (Docker down).
**Close gate:** PASS (--light)
**Cycle meter:** design 1/2 · fix 1 · implementer launches 1/5 (one agent, resumed for red → green → fix) · stage words 2258
**Numbers:** declared prod ~+20/−5 · actual +28/−5 (33 touched, 3 files) · tests ~+200 vs ~+95 declared · new public surface 1
**Owner decisions this stage:** none
**Debt carried:** none. Inner loop needs `-p:FletcherProbeShim`; `locate_shim.sh` refuses with 19 cached edge packages (shims picked by mtime, paths in evidence).
**Records corrected:** brief :63 variant-proof claim (0b); d10 NM citations shifted +11 (4b RECORD).
