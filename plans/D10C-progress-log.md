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

## D10C-3 — per-row Subscribe on the edge shim; a bad row is one drop (2026-11-15)

**Forcing test:** `SubscriberArrowTests.PerRowDeliversEachRowAloneOrCountsItAsOneDrop` (arms mixed, subscribedFirst, unopenable) → 🔴 → 🟢
**Lane:** full · **Design:** `plans/D10C-3-per-row-edge.md` · **Brief:** `plans/D10C-3-brief.md`
**What landed:** per-row `SubscriberArrow.Subscribe` runs on the inner `Subscriber` + `fl_decode_rows`, no batcher, so it works on the edge shim. One BC-16 site (`RowDelivery.OnRow`): every row is one handler call, decoded or null with `RowsDropped = 1` (:575 (3)). Codec released at the subscription's exactly-once free. One static `CopyAttachments` for rung 7 to reuse.
**Interfaces:** no public change; one internal `Subscriber.Subscribe` overload taking a release.
**Deleted:** none (the managed batcher goes at rung 8).
**Files touched:** `Subscriber.cs`, `SubscriberArrow.cs`; tests `SubscriberArrowTests.cs`, `ReentrancyTests.cs`; d10 doc, the bounded-context register.
**Reviews:** 0a CLEAR · 0b CLEAR · design APPROVE-WITH-DEBT(3) (all resolved) · compliance 2 blocking → PASS at cycle 2 · code review 0 blocking / 1 should-fix (S1 done) / 4 nits
**Verification:** dotnet 1492/1492 (net8+net10); pubsub-transport-conformance 34/34 (incl. `ASubscriberArrowForwardsItsReaderProfileOnBothForms`); binding-abi-conformance 11/11 ×2; pubsub-arrow-fastdds 4/4; gateway-dotnet-crosslang 11/11. **Not run:** `check_dotnet_packages.sh` and preview-bundle-consumer (both need the Linux shim; Docker down).
**Close gate:** PASS (full)
**Cycle meter:** design 1/2 · fix 1 · implementer launches 1/5 (one agent resumed: red, gaps, green, fix, comment) · stage words 6000
**Numbers:** declared prod 130–160 net (restated from +85) · actual +164/−9, net 125 excluding doc comments · tests +343 vs ~+160 · new public surface 0
**PM decisions:** (1) T38 not applicable here. (2) Per-row reason = `RowLimit` by elimination under L2 (0b: no ruling sets it). (3) Schema-less-first-row handling is defensive: spec §7 cl. 1 makes it unreachable, so no promise and no test (the internal-only leg was refused under law b). (4) The FastDDS test missed by the plans: its per-row half goes to rung 3, its batched half to rung 7.
**Owner decisions this stage:** none
**Debt carried:** none.
**Records corrected:** brief :18 (unreachable change), brief RowLimit basis, design :117 leg mapping and :142 numbers; d10 L9/§2/§4 marks and citations (4a RECORD, by the implementer).
