# D10C-2 — step 0a corpus audit (light lane)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test in scope. HEAD `af98a8b`.

**Proof-of-read:** `plans/RIX-rulings.md` read top to bottom: 603 lines, 45 `## ` entries, last "## 2026-11-15 — D10C ladder: the domain-7 port tests" (:595). d10 §3 L2 (:123-144), D26, D34, Q11; d9 read-only obligations via `binding.h:263-269` (`FL_ORIGIN_HANDOVER = 4`).

**Yardstick:** d10 L2 "Every number the shim hands out ... has a C# value equal to it, never renumbered"; ledger :551 (3) "origin `Handover` (4) ... C#'s origin enum gains code 4"; D34 "`BatchReason` is renumbered to the C values 1-3".

**Scope and classification:**
- `ErrorTests.*` (T44, `dotnet/tests/Fletcher.Tests/ErrorTests.cs`:42, 63, 89, 110, 136, 155, 170, 204, 260, 297, 338) — LAWFUL. Every origin and status is by name (`FletcherOrigin.Codec`/`.Seam`; the `(int)FletcherOrigin.Codec` fixture at :91 round-trips the enum's own value). None asserts or accepts an unnamed origin 4. KEEP; the stage adds Q11's table test beside it.
- `SubscriberArrowTests` (17 `BatchReason.*` sites, :284-1155) — LAWFUL for this rung: names only, no cast, `default(BatchReason)` or numeric reason anywhere, so the 0-to-1-3 renumbering is invisible to them. (Their managed-batcher vehicle is other rungs' concern.)
- No test in `dotnet/tests` or `integration-tests` pins a `FletcherStatus`/`FletcherOrigin`/`BatchReason` number, an unnamed origin 4, or the current 0-based reasons (searched: casts, `Origin = n`, `Enum.GetValues`, `FL_*`, `Handover`, `Variant`). No skipped/ignored tests in the two files.
- Out of scope, noted: `GatewayBatchReason` (`BatchedSubscriptionTests.cs`, `EndToEndTests.cs:118`) is the gateway client's own enum, not the C surface; d10 L2 does not reach it.

**Scheduled restoration:** none. **Tier-1 disagreement:** none (L2, D26, D34 and :551 (3) agree).

**Laws with no test:** L2 is `[built for fl_status, fl_origin 0-3]` with no test comparing either with `binding.h` (Q11, d9 Findings a7) — exactly the gap this rung's table test closes.
