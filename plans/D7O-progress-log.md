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
