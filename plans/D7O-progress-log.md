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

## D7O-2 — No timeout — 🟢 2026-11-12

**Landed:** `7a0cc64`, fix `6e5cd89`. `BatchOptions::kNoTimeout` (= `milliseconds::max()`, INT64_MAX
to a C caller): windows close only when full, on the offset budget or at closing; a window of only
lost rows is reported in the `kClosing` batch. A timeout too long for the clock arms no deadline
instead of an early, wrapped one (the `ArmTimer` overflow is gone). d7 §2 "Deadline" amended (owner
ruling, Decision 1 (a)); L4/L5 `[built]`; BC-4 updated.
**Steps:** 0a CLEAR · design `d60e8cd` · review APPROVE-WITH-DEBT(2) `53a37da` · Decision 1 ruled
`920b3ec` · phase A red: rung 2 compile failure on `kNoTimeout` only; rung 3 `milliseconds::max()`
and clock-max arms each got `kTimeout:1 rows` (row 6, the wrapped deadline), 3/3 runs, no crash;
100-year arm green both ways (guard); 10 ms control green, and the forcing tests with 10 ms fail
every arm · 0b CLEAR (one unmoored point: lost rows never count toward `max_rows` rests only on
D-D7O-5/F2) · green 55/55 · 4a ISSUES(2, 1 blocking: README sentence) → fix 1 → cycle 2 PASS · 4b 0
blocking, 3 nits.
**Verification:** inner loop `conan create pubsub-arrow` run_tests 55/55 (implementer, PM-checked
log); full suite: `ctest -L heavy` 2/2, pubsub-arrow-fastdds 4/4 (PM).
**Numbers:** production ~+15/−6 vs declared +10/−2 (**close finding:** >50% over; `static_assert`
message, `<ratio>`, a two-line `constexpr`); tests +277; design cycles 1; fix cycles 1; implementer
launches 1 (resumed for phase B and the fix); owner touches 1.
**Decided by PM:** header `hpp:121` narrowed to "any other timeout" stays; restoring "value" booked
into D7O-3 (4a N1). README loss sentence reduced to a pointer rather than re-worded.
**Logged, not gated (4b nits):** the timeout-to-clock conversion could be settled once in the
constructor; a second `Observe()` after `End()` cannot fail (~1.4 s of sleep); no test combines no
timeout with an offset-budget close (that path never touches the timer).

## D7O-3 — Any row count is used as given — 🟢 2026-11-12

**Landed:** `c18e95e`, fix `57aa4bb`, records `1434325`. Only the `BatchDecoder` constructor decides a
schema is unusable; the decoder pre-size is bounded at min(`max_rows`, 65,536), and a failed
pre-size resets the builders and decides nothing. `max_rows` and the timeout are stored `const` and
used as given. BC-8 sweep (42 cells) built. Header restores "Any other value is used as given" and
discloses the bounded pre-size and the memory-loss residue.
**Steps:** 0a CLEAR · design `00dacb8` · review APPROVE-WITH-DEBT(4) — both brief Decisions were
already answered by law (Decision 1's option (b) broke L10), so they became disclosures and no owner
question was asked (`957a514`) · phase A red: (a) null `kTimeout` dropped 3, (b) null `kClosing`
dropped 3, DEBT-3 cell null batch (`Reserve` OOM), sweep exactly the 5 accepted INT64_MAX cells; (c)
guard green · 0b CLEAR · green 58/58 · 4a CONFORMS ISSUES(2, 0 blocking) · 4b 0 blocking, S1
should-fix taken → fix 1 → 4b cycle 2 clean.
**Verification:** inner loop 58/58 (PM run, and implementer after the fix, log
`scratchpad/d7o3_fix1_conan.log`); heavy 2/2, pubsub-arrow-fastdds 4/4 (PM, before and after the fix).
**Numbers:** production +35/−18 vs declared +16/−10 (**close finding**: >50% over, mostly comments);
tests +414 vs ~+190 estimated; design cycles 1; fix cycles 1; implementer launches 1 (resumed for
phase B and the fix); owner touches 0.
**Found and recorded, not fixed (open):** a memory failure while *constructing* the decoder still
makes a good schema unusable for life (d7 Q8, Findings (d), BC-3 registered gap). A `Finish()` that
throws right after a failed pre-size leaves later columns un-reset (untested residue).
**Logged, not gated:** 4b N1 (guard does not prove the pre-size failed; ASan would abort), N3 (arm
(c)'s 131,074 publishes could move to the heavy file), N4 (a `ResetBuilders()` helper); 4a N1/N2
taken. Domain 2 note: `bench_arrow_codec.cpp:388-390` comment stale since D7F (not edited).
