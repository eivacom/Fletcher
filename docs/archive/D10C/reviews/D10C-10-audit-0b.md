# D10C-10 step 0b new-test audit (rung 11)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test; no scheduled restoration; no tier-1 disagreement; no new VALUE subject beyond rung 11. Uncommitted tree on `2d5063c`.
**Proof of read:** `plans/RIX-rulings.md` top to bottom, **684 lines, 54 `## ` entries**, last `## 2026-11-17 — Fletcher Linking Exception: legal approval; pack it` (:677; does not bind). d10 §3 L1-L15 read.
**Yardstick:** :383 (1) "never lost silently"; :512 "borrowed for the callback"; :551 (3) "null batch, its rows counted, and the failure's status, origin `Handover` (4) and message"; :669 "when C# itself fails to take over a window, the handler gets a null batch with the rows counted and no error"; :525 (renamed-copy staging is "test staging only"); d10 L2, L3 ("forwards a lower layer's answer; it does not answer again"), L8 ("never disposes a batch's `failure`"), L9, L14, D26.

**Scope, all LAWFUL:**
- `HandoverTests.AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure` (A, B; `HandoverTests.cs:104`): asserts :551 (3) and d9's promise (Internal, Handover) end to end, plus conservation (:383 (1)) and one Closing last (:484). `fired == 1` makes it non-vacuous.
- `UnarmedNoWindowCarriesAFailure` (:146), the control. `arm(0)` is sound: `handover_fault.cpp:48-51` zeroes `g_fired` and leaves `g_remaining` 0, so the fault is disarmed. Without it, a leftover `fired == 1` from an earlier cell in the same process would turn the control red falsely. It arms nothing, so it still proves "C# invents no failure".
- `SubscriberArrowTests.TheBatchThunkForwardsAFailureAsGiven` (:2144): forwarding a (PayloadTooLarge, Seam) pair is what L3 requires. It pins the C# side and does not license the shim to send it. `IsType<FletcherException>` holds: D-BIND-54 picks the format type only for (Codec, InvalidArgument), `Errors.cs:70`.
- `.TheBatchThunkNeverDisposesTheBorrowedFailure` (:2204) and its source scan: L8, :512. Internal by nature, as the 0a audit and design review accepted.
- `.ACopiedFailureThatCannotBeDecodedStillDeliversTheWindow` (:2254): L9's "the window arrives", with `Failure == null` under :669. The brief's "Handled" line ("no error attached") was in front of the owner when :669 was ruled.
- `.AWindowTheThunkCannotTakeOver…` (:2061), adding `Assert.Null(Failure)`: this is :669 verbatim.
- `ErrorTests.cs:104-105` is prose only and accurate. `Fletcher.slnx` +1 and the csproj staging: see law (b).

**Law (b): legitimate.** Ladder rung 11 names this exercise: "server file is d9's test-only hand-over fault shim (staged under the server name; test staging only)". :525 is the precedent for renaming a file for staging. The shipped path runs from `SubscribeBatched` through domain 7 to the shim's hand-over. The missing property fails the tests and never skips them. A wrong file fails `fired == 1`.
**Vacuity:** the message check can still fail. It fails on a null `Failure`, and on a message that never crossed (the fallback). The leg at :2144 carries byte-exactness through the same decoder.
**Laws with no test:** none new. L8's `failure` clause, and the hand-over halves of L9 and L14, gain tests here. Their marks stay `[designed-not-built]` until green (no false green).

**Notes (non-blocking, test strength, not authority):**
1. `HandoverTests.cs:53` copies the fallback text from `Errors.cs:89`. If that text drifts, the `NotEqual` goes vacuous without failing.
2. The scan (:2204) matches `NativeMethods.fl_error_dispose` in `SubscriberArrow.cs` only. The design asks for any reference. A copy-dispose inside `Errors.ToException` (an `in` parameter) escapes both the predicate and the scan.
3. :2254 pins "no failure at all" when the message cannot be copied. Its basis is :669, read with the brief's Handled line. d10 L9's ingest should name the copy case explicitly.
