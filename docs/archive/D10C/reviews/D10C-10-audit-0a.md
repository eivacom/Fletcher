# D10C-10 step 0a corpus audit (rung 11: a failed hand-over reaches C# in BatchStatus)

**Verdict: CLEAR.** HEAD `558a879`. No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test in scope; no scheduled restoration; no tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom, **667 lines**, 52 `## ` entries, last `## 2026-11-16 — D10C-7: the switch and the deletion are one stage` (:660).
**Yardstick:** :383 (1) "A batch the shim cannot hand over is reported, never lost silently"; :512 "the no-batch delivery carries the failure's status and message, borrowed for the callback"; :551 (3) "A failed hand-over reaches a C# handler with a null batch, its rows counted, and the failure's status, origin `Handover` (4) and message"; :246 (2) (handler throws caught and counted in C#); d10 L2, L8 ("never disposes a batch's `failure`"), L9, L14, D26, §1; d9 L2, L18, D30, D32; register BC-12.

**Scope covered (all classified LAWFUL; none ignored or disabled, no `Skip`, no `#if false`):**
- `SubscriberArrowTests` (2554 lines): every `BatchStatus` reader (Reason/RowsDropped arms, :390-2456), the two thunk legs `TheBatchThunkMovesTheBatchOutOfTheBorrowedStructs` (:1980) and `AWindowTheThunkCannotTakeOverArrivesAsANullBatchWithItsRowsCounted` (:2061), the handler-fault pair `AHandlerThatThrowsIsAbsorbedCountedAndRaised` (:2426) / `ABatchedHandlerThatThrowsIsAbsorbedCountedAndRaised` (:2455), the `Delivery`/`Sink` fixtures (:2497-2553). None reads or asserts `fl_batch.failure` (both legs leave it 0) and none asserts a null batch is silent or uncounted.
- `ErrorTests.OriginFourIsHandover` (:108), `AbiNumberTableTests` `fl_origin` row (:57): L2 number mapping, not the delivery path; not "raised differently".
- `ProcessWideTests.AbsorbedTotalMovesByExactlyWhatTheSubscribersAbsorbed` (:165), `ThunkDisciplineTests.*` (:54, :104): L9/L10 counting and containment.
- `Fletcher.CopyOracle.Tests` (5 tests, `ProbeMethods` fixture) and its csproj staging: probe shim, copy oracle; nothing on hand-over. `Fletcher.TestHost`, `Shared/LoadedShim.cs`: no hand-over subject.
- d9, read only: `c-abi/tests/test_batches.cpp` `AWindowTheShimCannotHandOverStillArrivesCounted` (:1539, cells k=2 RL, k=3 CLOSING), its fixture `Seen.had_failure`/`failure_*` (:109-170), `AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted` (:549); `c-abi/tests/handover_fault.cpp`; `tests/CMakeLists.txt:210-269`. d9 promises: null array/schema, `num_rows` 0, `rows_dropped` = window rows, reason kept, `FL_INTERNAL` + `FL_ORIGIN_HANDOVER` + message, no failure on other windows, one CLOSING last, `fired == 1`.

**Laws with no test:** d10 L8 is `[built]` but its "never disposes a batch's `failure`" clause has no test (vacuous while `failure` is unread); L9's and L14's hand-over halves are honestly `[designed-not-built]` (no false green). Rung 11 owns all three.

**Notes for design (not findings):**
1. Loading the fault shim from C#: follow CopyOracle's pattern (own test project, a csproj property staging the file under a shipped name; one shim per process, d9 L17), but under the **server** name, so `NativeLoader` prefers it (d10 L5). It is built only in `c-abi`'s test build, server variant (`CMakeLists.txt:219-231`); the probe option is refused with `variant=server`.
2. d9 asserts the message equals `std::bad_alloc().what()`, which differs by C++ runtime; a C# test cannot spell it portably. Compare it by non-empty and verbatim to what the shim reports, not by a literal.
3. D10C-7 residue (a) (C#'s own take-over failure, :2061 leg) carries no failure today; whether it gains one in `BatchStatus` is the design's call. The leg asserts neither way, so no tolerance finding.
4. `ErrorTests.cs:104-105` remark "no managed call can provoke a failed hand-over on demand" goes stale at rung 11 (prose only).
