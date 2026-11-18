# D9S-6 compliance review (step 4a)

## Cycle 1: ISSUES (1 blocking)

**Proof of read:** `plans/RIX-rulings.md`, read top to bottom: 514 lines (`wc -l`), 39 entries. The highest is at
l.504, "2026-11-14 — D9S-6: a failed hand-over still carries its status and message". :383, :445(2) (expired), :478
and :504 govern. No implicit collision between rulings; no tier-1 disagreement. **Scope:** `d43eb0b..abc0776`, 11
files, judged against the design, brief, review cycle 2, DEBT-4/5, D-D9S-4/6/8 and rung 8 / l.93. Not built (on instruction).

**F1 (BLOCKING; a STOP-AND-ASK if it is kept as law): d9 L18's "Settled" bullet adds law that no ruling gives.**
d9:440-441 reads "Every window reaches the callback exactly once; a callback that throws anyway is swallowed after
it has its window." The bullet cites D32 and ruling :504, and neither says this. The source is design step 2, a
tier-3 "handled, not forbidden" choice. The brief says outright that no ruling grants either crashing or swallowing.
In §3, the sentence obliges the shim to swallow, so a later change to termination would break L18. That is a law
change without a ruling (d9 l.7).
*Fix:* drop the swallow clause from L18. Re-cite "exactly once" to L15/BC-9 + L18. If the swallow should be
recorded, put it under L1's "other direction" bullet as non-normative text: "the batch thunk swallows a throwing
callback after it has its window (handled, not ruled; D9S-6 design step 2)". Keeping it as L18 law needs an owner question.

**The six pressure tests:**
1. *L2 and D32 against :504.* **The L2 clause (d9:170-173) is exactly :504:** borrowed for the callback, copy to
   keep, never dispose, the shim disposes it after the call (what "borrowed" means), and a returned failure stays
   the caller's (L2 restated). **D32** = :504's field and code + no status number. It also carries design shape
   (trailing, NULL unless failed), D-D9S-6 (the locked 0.8 bump) and DEBT-4(c)'s forward note to domain 10. None of
   that is new law. The §1 parenthesis is :504's "d9 §1 holds". L2's headline is rightly untouched: :504 authorises
   a clause, not a new headline. The one excess is in L18: see F1.
2. *D-D9S-4 holds; the stop-and-ask is not breached.* In the shipped `batches_server.cpp:43-48`, the point is
   `((void)0)` without the macro, and the forward declaration sits inside the `#ifdef`: no symbol, call or export.
   The macro is PRIVATE to `fletcher-c-abi-handover-fault`, which exists only under `FLETCHER_BUILD_TESTS` (set by
   Conan only with `run_tests`). `fletcher-c-abi-impl` does not hold `batches_server.cpp`, so each target compiles
   its own copy (P1). `handover_fault.cpp` has an `#error` guard. The macro on the shipped target gives an unresolved
   `HandoverFaultPoint` (M6). `package()` names the stem, and `...-handover-fault` does not match (held by name only,
   DEBT-1). `check_shim_exports.sh` refuses any `fl_test_*`. The seam lives in a test artefact (ladder Findings 3).
3. *`std::abort()` fits d9 L1.* `ReasonOf` is `noexcept`, and its tail is unreachable: the switch is exhaustive,
   with `/we4062` / `-Werror=switch` on both targets. Reaching it is termination, not an exception crossing, and the
   d9:146-149 note says exactly that, citing DEBT-2. Nit: the code comment at `batches_server.cpp:79` cites L1 for
   "rather than throw into domain 7". That rule is L18/D-D9S-4.
4. *Nothing survives that should not* (tree grep). Gone: the empty `catch` around the whole hand-over, the "Until
   D9S-6 … lost uncounted" comment, `ReasonOf`'s `throw`, L18's "Ambiguous" note, §4's "Not yet built", Q12's
   "half open" and BC-12's `[designed-not-built]`. The remaining `catch` (`:165-168`) wraps only `on_batch`, as
   design step 2 orders. `handles.hpp` is DEBT-5's fix and is correct. DEBT-4(a)(b)(c) landed.
5. *`[built]` marks.* Each new mark names `handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`: §2
   Hand-over, the L2 clause, L18, §4 `failure` and BC-12. The test matches design §Forcing-test: both cells, every
   assertion, arm before publish, `fired == 1`. Two marks overreach (RECORD).
6. *Scope.* No diff under `dotnet/`, `pubsub-arrow/`, `arrow-bridge/`, `core/`, d7 or d2. No edge answer changes:
   the edge build never delivers a batch, and `handles.hpp` changes only the out-of-memory path. No window, count,
   timer or thread: the BC-9 scan finds nothing new in `src/`, `offered` is a local, and the atomics live in `tests/`.

**Corner-case ladder:** survives. One call site after the `try`. The fallback is a fresh `fl_batch`. The reason and
rows are fixed before the attempt. The catch is `noexcept` (`Capture`) and never rethrows. No retry.

**RECORD:**
- d9:173: the L2 clause's `[built]` names a test that cannot observe "the shim disposes it when the callback
  returns". Narrow the mark as L14 does: disposed by structure, ASan/LSan owed.
- BC-12's test column and d9:435-436 say "the export fails on window k". The test throws `bad_alloc` after the export.
  The failed-export path is proven by structure only (0b note 1).
- Design Files-to-touch omits `c-abi/src/handles.hpp`, which DEBT-5 added (the register allowed it).
- Ladder rung 8 (l.63) does not name the `failure` field or code 4, as :504 binds. Rung 4's l.59 "no later rung
  changes a signature" is unamended (DEBT-4 PM note).
- `plans/D9S-evidence.md` has no D9S-6 row. The "Windows, by hand" marks depend on it at close.
