# D7O — Progress log

## Planning — 2026-11-12

Round designed by `round-designer` (resumed after a session crash that lost the earlier answers
before they were recorded; they are now in the ledger, marked as recorded late). Owner rulings that
day: one batcher, C++'s; one shim in two variants; no interim C# fixes, and C# does not ship to
`main` first (an implicit collision, revised in place); "never" added to C++; out-of-range options
refused; the shim is its own domain; BC-2 and BC-7 stay deferred. Domain 7 amended (L4, L5, L15,
D14–D16, Q5); register BC-8 and BC-9 added; domains 9 and 10 named `anticipated` in the map.
Audit: 28 rows, 0 BLOCKER. Ladder: 4 rungs, 3 stages.
**Hand-over:** LC-1 confirmed ("Keep it"); ladder Q1 accepted ("Fix in D7O"). Planning committed;
branch `feature/d7o-batch-options` cut and pushed.

## Kickoff — 2026-11-12

**Directive (owner):** "Run all three items". Start at D7O-1 and continue through D7O-3, stopping
only for a Stage Brief with decisions, a stop-and-ask or a blocker. Commit and push each item to
`origin feature/d7o-batch-options` at close. Full loop (no light lane in the config).
**Preflight:** all required config keys present; purpose passes the strike test. Plugin source 0.10.0
vs marketplace 0.9.1, as in D7F; skills load from the source checkout.

## D7O-1 — Nonsensical options are refused — 🟢 2026-11-12

**Landed:** `9abb0c7`. `max_rows < 1` or a negative timeout makes the batched `Subscribe` throw
`PubSubError(kInvalidArgument)` from `RecordBatchBatcher`'s constructor, before the batch thread
starts or any provider subscription exists; the clamp is deleted. Header and README state the
refusal; d7 L15 `[built]` for the refusal; BC-8 check `[built]`, sweep owed by D7O-3.
**Steps:** 0a CLEAR · design `16f3d7e` · review APPROVE-WITH-DEBT(1) `555d899` (placement in the
constructor, not at the top of `Subscribe`: judged conformant) · phase A red (24 assertions; Mock
counter observed 1 on the red run, DEBT-1) · 0b CLEAR · green · 4a ISSUES(1, 0 blocking) · 4b
0 blocking, 4 nits.
**Verification:** inner loop `conan create pubsub-arrow` run_tests, 52/52 (implementer log
`scratchpad/d7o1_conan.log`, PM-checked); full suite: see the evidence ledger.
**Numbers:** production +11/−2 against declared +6/−1 (**close finding:** >50% over declared adds;
an include and a longer message; owner note requested at round close); tests +165; design cycles 1;
fix cycles 0; implementer launches 1 (resumed once for phase B).
**Record corrections (PM, in place):** d7 header L15 mark; L15 citation `cpp:34-47`; "rung 1"; a
note that D7O-1 shifted later `cpp:N` citations by ~9 lines; DEBT-1 struck.
**Logged, not gated:** 4a N1 (header "used as given" untrue for the two huge values until D7O-2/3;
branch stays off `main` until D7O-3). 4b nits: sinks declared after `sub` in two tests (destroyed
first if a wait fails), `weak_ptr` expiry relying on a moved-from `std::function` being empty, a
200 ms sleep that only serves the red run, a `std::cerr` line on every pass. 4a records: C# keeps
its own check until the switch round (BC-8's binding clause unmarked).
