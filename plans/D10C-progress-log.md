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
