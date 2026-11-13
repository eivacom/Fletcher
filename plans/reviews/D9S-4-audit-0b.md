# D9S-4a — step 0b test-authority audit (MODE=new-test)

**Verdict: CLEAR.** No test in scope is FORBIDDEN or FORBIDDEN-BY-TOLERANCE. No tier-1 disagreement. Nothing schedules a restoration.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **459 lines**, last heading **"2026-11-13 — D9S-4: one subscriber object; the interim handoff gap accepted"** (l.445). Also d9 §3 (L1–L18), d7 §2–§4 (L1–L15, D1–D16), register BC-8 to BC-10, ladder rung 4 (and 5–8 for what is owed later), the design and the brief, and `D9S-locked-decisions.md`.

**Scope** (uncommitted): `c-abi/tests/test_batches.cpp` (the reason `static_assert`s at l.44-46, the helpers `BatchSink`, `PublishedRows`, `BatchFixture`, and three tests), `c-abi/tests/test_shim_batches_nothing.cpp` (the guard and its control), and the `c-abi/tests/CMakeLists.txt` diff.

| Test | Class | Basis |
|---|---|---|
| `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` (l.272) | LAWFUL | d9 L14, L15; d7 L4, L12, D10. Unmoored but design-backed: bit 63 on the id (B1) and the reason values 1/2/3 (B2). |
| `Batches.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted` (l.349) | LAWFUL | d7 L6, L7, L4; d9 L14. Flag (2), below. |
| `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` (l.420) | LAWFUL | d9 L12, L13, L3; D28; BC-10. One answer per build. Flag (1), below. |
| `ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer` (l.66) | LAWFUL (guard) | d9 L11, L15; BC-9; ruling 2026-11-12 "one batch assembler". |
| `ShimBatchesNothing.TheMatcherFindsEveryPatternItIsGivenInItsOwnSource` (l.90) | LAWFUL (control) | Proves the matcher can say yes. |

**Flag (1): server arm, NULL subscriber gives `FL_INVALID_ARGUMENT` / `FL_ORIGIN_SEAM`, OUTs untouched.** Lawful.
- d9 L3: "OUT parameters are written only on success."
- d9 L13: the server build behaves as the row subscribe does.
- Design "Rung 2 (refused, typed, at the door)" names this exact refusal, so it is not a new value subject.
- It is the control that makes the edge arm's NULL-subscriber row mean "reads the build first".
- No tier-1 text fixes the *origin*, for this case or for the edge refusal (d9 L2/L12: "Its `fl_origin` is open (Q12)"). Those two origin assertions are therefore UNMOORED, resting on the design's Q12 resolution. The design owes d9 Q12's edge half closed, so the law catches up with the test.

**Flag (2): every no-batch delivery has `reason == FL_BATCH_TIMEOUT` and `rows_dropped > 0`.** Lawful, but not for the reason the test comment gives.
- **TIMEOUT follows from d7 L4 alone.** L4: a window closes "at the first of four events: `max_rows` rows, the deadline … the offset budget, or closing". The test publishes 3 rows against `max_rows` 100, there is no decoder (so no budget), and nothing closes before the snapshot. The deadline is the only event left. The comment's mechanism ("a window of lost rows never counts toward max_rows", a d7 L1 bullet) is not load-bearing. Suggest the comment cite L4 instead.
- **`> 0` follows from d7 L6:** "A window with nothing pending delivers nothing", so a delivery with no decoder exists only because of drops.
- **No timeout:** the test uses 200 ms, so nothing conflicts. Had it used `INT64_MAX`, a TIMEOUT expectation would be forbidden: L4 has "the deadline (only for a subscription that has a timeout)", and L1 says a lost-only window "is reported only in the `kClosing` batch".

**Law (a) and L18.** No test needs L18 (D9S-6, ruling 2026-11-13 (2)). No fault is injected, there is no `failure` field, and no test accepts a lost hand-over (so no tolerance either). The rung test pins exact boundaries [2,2,1] and ROW,ROW,TIMEOUT. d7 L4/L5 promise these only if the publish of row 2 reaches the batcher within 1.5 s of the subscribe: `last_flush_` is set at construction (`subscriber_arrow.cpp:320`), and the deadline is `max(now+1ms, last_flush+timeout)`. That is a timing premise, not a law violation. **The design's and comment's claim "only the reason assertion can flake, never conservation" is inaccurate:** a stall over 1.5 s also changes the delivery count and the slices. Restate it.

**Law (b).** Every behavioural call is `fl_*` through `binding.h`, and batches are imported with the test binary's own Arrow. All waits are one-sided upper bounds. The edge arm's "nothing in 2 s" has a server control.

**The guard is structural over files, not over patterns.**
- It recursively scans every source file under `c-abi/src`, with no site list and no exemption.
- The patterns are a closed deny-list. `pthread_create`, `CreateThread`/`_beginthread`, `std::future`/`wait_for` and OS timers (`timer_create`, `steady_timer`, `SetTimer`) would pass unseen.
- BC-9's stated test also names "a timer". None of these strings appears in `c-abi/src` today, so adding `timer`, `pthread`, `CreateThread`, `_beginthread`, `wait_for` and `std::future` costs nothing.
- Non-blocking.

**Laws vs marks (false-green risk at close, not a test defect).** The design's Docs line says "d9 … L14 and L15 marked `[built]`". 4a has no test of three clauses:
- L14's "what the binding has not moved … the shim releases then". The sink always moves; this is ladder rung 5.
- L15's options pass-through and refusal. Rung 7, BC-8/BC-14.
- L15's "closing follows domain 7's L9". Rung 6.

Mark those clauses partial at the 4a close. Otherwise they are a false green.
