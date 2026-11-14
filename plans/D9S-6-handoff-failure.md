# D9S-6 — A batch the shim cannot hand over still arrives, counted (design)

*Step 1, cycle 2, HEAD `4b9136f`. Rung 8; d9 L18 (D30), BC-12, D-D9S-4, d9 Q12 (hand-over half). Ledger read in full:
514 lines; last entry l.504 "2026-11-14 — D9S-6: a failed hand-over still carries its status and message". 0a: CLEAR.*

## Summary
L18's one site goes in the server build's batch thunk. A window the shim cannot hand over still reaches the binding,
once, as a no-batch delivery: rows counted lost, domain 7's reason kept, CLOSING included. Under d9 §1 (ruling l.504),
the failure crosses with it as status, `FL_ORIGIN_HANDOVER` and message, borrowed for the call. The new surface is one
field and one enumerator. The fault hook lives in a test-only shim that is never packaged.

## Design
**1. Two phases, one call** (`batches_server.cpp:100-140`; today one `try` holds both and swallows all):
```cpp
const int32_t reason = ReasonOf(status.reason);                 // noexcept (step 3)
const int64_t offered = batch != nullptr ? batch->num_rows() : 0;
std::optional<std::deque<fl_attachments>> sets;                 // empty optionals: no allocation
std::optional<std::vector<const fl_attachments*>> set_ptrs;
ArrowArray array = {}; ArrowSchema schema = {};
const ReleaseUnmoved release_unmoved{&array, &schema};          // BC-13's one release, every exit
fl_error failure = {};  fl_batch out = {};
try {                                                           // THE HAND-OVER: everything or nothing
    sets.emplace(); set_ptrs.emplace();  /* today's steps, unchanged */
    FLETCHER_HANDOVER_FAULT_POINT();                            // empty outside the test shim
} catch (...) {                                                 // d9 L18, BC-12: the one site
    Capture(&failure, FL_ORIGIN_HANDOVER);                      // noexcept; the one containment site
    out = fl_batch{reason, status.rows_dropped + offered, 0, nullptr, nullptr, nullptr, &failure};
}
try { on_batch(ctx, &out); } catch (...) {}                     // step 2
fl_error_dispose(&failure);                                     // the shim's, after the call (step 5)
```
**Nothing before the `try` can throw:** only `noexcept` calls, empty optionals and trivial aggregates; both containers
are built inside it. The catch cannot throw: `Capture` is `noexcept` and allocates with `nothrow` (`containment.cpp:29-39`).
- **The fallback is written fresh.** What the attempt built is released after the call, like an unmoved batch (BC-13).
- **`on_batch` runs exactly once per window**, outside the `try`. This keeps ruling :478 for CLOSING.
- **The loss sum is L18's own wording.** It keeps no state, so L15/BC-9's "no count" holds.

**2. The callback's own throw** breaches `h:1042` after the hand-over, so nothing is lost; the guard keeps L18's "never throws back" true.

**3. `ReasonOf` becomes `noexcept`:** the exhaustive switch's unreachable tail becomes `std::abort()` (0 never crosses, `h:1004`).

**4. The fault hook (D-D9S-4): test-only, and absent from every shipped artefact.**
- **`c-abi/tests/handover_fault.cpp` (new).** `#error` unless `FLETCHER_C_ABI_HANDOVER_FAULT` is set. It exports
  `fl_test_handover_fault_arm(int64_t nth)` (the nth hand-over from now fails, once) and `fl_test_handover_fault_fired()`.
  On the armed hand-over, the point throws `std::bad_alloc`, after the export. The hook's state is atomics.
- **`batches_server.cpp`.** Under the macro, the point calls into that file; otherwise it is `((void)0)` (about 6 lines).
- **`c-abi/tests/CMakeLists.txt`, server only:** a SHARED `fletcher-c-abi-handover-fault` shaped like the probe
  block (`CMakeLists.txt:227-259`): the shipped shim's four sources plus the hook; the same visibility, version
  script and SERVER variant; switch-as-error, `pubsub-arrow` and the macro. `c_abi_handover_tests` compiles
  `test_batches.cpp` with `FLETCHER_EXPECTED_VARIANT=2` and `FLETCHER_HANDOVER_FAULT_TESTS=1`, prefixed "handover.".
- **Why it cannot ship:**
  - (i) the target exists only in `tests/` (`FLETCHER_BUILD_TESTS`);
  - (ii) the macro and the hook are on that target alone, and CMake compiles `batches_server.cpp` once per target;
  - (iii) `package()` copies the shipped stem by name (`conanfile.py:163-179`): **held by name only** (DEBT-1);
  - (iv) `check_shim_exports.sh` fails on any shipped `fl_test_*` export. It checks exports, not package contents.

**5. The failure crosses (Decision 1, answered).** The owner chose "(b) Keep the promise (Rec.)"
(ruling l.504, "D9S-6: a failed hand-over still carries its status and message"): d9 §1 holds.
- *Surface.* `fl_batch` gains a trailing `const fl_error* failure`, NULL unless this window's hand-over failed.
  `fl_origin` gains an appended `FL_ORIGIN_HANDOVER = 4`. Both ride on D9S-1's 0.8 bump, as the 4a and 5a
  additions did (D27). No status number is added.
- *Ownership.* **Borrowed for the call**, like all of `fl_batch` (L11, `h:1011`). A binding copies the message to
  keep it and never disposes it; the shim disposes it after the callback. d9 L2 gains the authorised clause: a
  failure handed to a batch callback is borrowed. One an entry point returns stays the caller's.
- *Memory exhaustion.* `Capture` allocates with `nothrow`, so the message may come out empty. The status
  and origin still cross (`containment.cpp:35`).

**6. Texts to amend** (docs in the code commit; the PM does the plan lines):
- d9 L2: the borrowed-failure clause (ruling l.504).
- d9 §1: the hand-over failure, named under "status plus message".
- d9 §4: the "Not yet built: `failure`" line becomes the built field.
- d9 L18: `[built]`; its "Ambiguous in the ruling" note is replaced by the settled shape.
- d9 Q12: the hand-over half is settled as `FL_ORIGIN_HANDOVER` (4).
- d9 L1: the `ReasonOf` abort (DEBT-2).
- d9 §2: the Hand-over term becomes `[built]`.
- Ladder rung 8: the origin is code 4 and rides on D9S-1's bump (PM).
- The plan's "no batch-layout change after stage 4" line, amended as the ruling says (PM).
- Register BC-12: `[built]`.
- `binding.h`: docs for the `failure` field, `FL_ORIGIN_HANDOVER`, and the `fl_batch` no-batch paragraph
  ("or the shim could not hand it over, then `failure` says why"). `c-abi/README.md` to match.

## Corner cases forbidden
1. **One window delivered twice.** Rung 1: there is one call site, after the `try`.
2. **A no-batch delivery carrying a half-built array, schema or attachments.** Rung 1: the fallback is a fresh struct.
3. **The reason or CLOSING lost.** Rung 1: fixed before the attempt; a fourth enumerator fails the build (step 3).
4. **Loss-count overflow.** Rung 1, by d7 L2: the sum is the window's rows, already an `int64_t`.
5. **A hand-over throw reaching domain 7.** Rung 1: "nothing before the `try` can throw" (step 1), and the
   catch is `noexcept`. Rung 2: one catch, no rethrow.
6. **A retry, or a partial batch.** Rung 2: one attempt, all or nothing.
7. **The hook in a shipped artefact.** Rung 1: 4 (i)-(ii). (iii) is held by name, and (iv) checks exports only.

**Handled:** a throwing callback (step 2). *Why not forbidden:* foreign code (L1); terminating is unruled behaviour.

## Premises and stop conditions
- **P1** CMake compiles a source once per target. If the fault shim could only share the shipped object: **STOP-AND-ASK**.
- **P2** `package()` takes the shipped stem by name (DEBT-1). A matching pattern: rename. None escapes: **STOP-AND-ASK**.
- **P3** Domain 7 calls once per window, in order (d7 L12), and a return means delivered. Otherwise **STOP**.
- **P4** Each TEST runs in its own process, and the hook counts per process from its own arm call.
- **P5** With `max_rows` 2 and `INT64_MAX`, 5 rows give RL(2), RL(2), CLOSING(1). If not: stop and report; never loosen.

## Forcing-test mapping
**`handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`** (`test_batches.cpp`, under the test macro;
reuses `BatchFixture`, `PublishedRows`, `BatchSink`, `CloseBatches`). Cells k = 2, 3, each with **exactly one batched
subscription** (DEBT-3, applied): publish 5 rows, arm hand-over k, close from outside. Each asserts:
- 3 deliveries, RL, RL, CLOSING, with exactly one CLOSING, and it last;
- delivery k: no array, schema or attachments; `num_rows` 0; `rows_dropped` equal to its window's rows (2 or 1);
- delivery k's `failure`: `FL_INTERNAL`, `FL_ORIGIN_HANDOVER`, message == `std::bad_alloc().what()` (copied in the callback);
- every other delivery has `failure == NULL`;
- every other delivery equals what was published (`ExpectWindows`);
- Σ`num_rows` + Σ`rows_dropped` == 5, and nothing arrives after close returns (2 s);
- `fired == 1` (internal; it only guards against a vacuous run).

**Red, pre-change** (hook, artefact and test land first): k is swallowed; k=2 Σ 3 ≠ 5, k=3 no CLOSING. **Green:** step 1.

**Mutations, each red:**
- M1, the empty catch restored: both cells.
- M2, `+ offered` dropped: Σ and k's `rows_dropped`.
- M3, a constant ROW_LIMIT in the fallback: k=3 has no CLOSING.
- M4, `out.array` kept in the fallback: k's `had_array`.
- M5, the macro off `batches_server.cpp` alone: `fired == 0`.
- M6, the macro on the shipped target: a link error, or `check_shim_exports.sh`.
- M7, `out.failure` left NULL in the fallback: k's failure check.
- M8, `FL_ORIGIN_SEAM` instead: k's origin.
- M9, `failure` set on every delivery: the NULL check.

**Controls:**
- C1: the whole `Batches` suite runs unarmed in `c_abi_handover_tests`, green. The hook is inert.
- C2: `c_abi_tests` stays green on edge and server. No edge or row answer changes (L12, L13).

## Risks / Unknowns
- **A half-built export's release is proven by structure, not measured.** The ASan/LSan leg is owed (D9S-5a DEBT-1).
- **The header layout changes for both builds.** The edge build never delivers a batch, so no edge answer changes.
- **C1 adds about 20 s to CI.** No coexistence window. Domains 7 and 2 and `dotnet/` are untouched.

## Files-to-touch
- **Code:** `c-abi/src/batches_server.cpp`.
- **Header:** `c-abi/include/fletcher/abi/binding.h` (`failure`, `FL_ORIGIN_HANDOVER`, and their docs).
- **Tests:** `c-abi/tests/handover_fault.cpp` (new), `c-abi/tests/CMakeLists.txt`, `c-abi/tests/test_batches.cpp`.
- **Docs:** step 6's texts, in `docs/domains/d9-c-abi-shim.md`, `bounded-context-register.md` and `c-abi/README.md`.

## Files-to-delete
- **The thunk's empty `catch (...) {}` and its comment** (`:103-106`, `:138-139`), replaced by the L18 site.
- **`ReasonOf`'s `throw`.** Replaced by `std::abort()`.
- **d9 L18's "Ambiguous in the ruling" note.** Replaced by the settled shape (step 6).
- **Tests:** none. 0a found no test that asserts the gap.

## Lines (production counts toward the 250 ceiling; the seam and the tests are test code)
- **Production:** +50 / −16 (`batches_server.cpp` +37/−15, `binding.h` +13/−1).
- **Test-only seam:** +75 (`handover_fault.cpp` ~30, `tests/CMakeLists.txt` ~45).
- **Tests:** +130 (`test_batches.cpp`).
