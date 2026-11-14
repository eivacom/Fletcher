# D9S-6 — A batch the shim cannot hand over still arrives, counted (design)

*Step 1, full loop, HEAD `4b9136f`. Rung 8; d9 L18 (D30), BC-12, D-D9S-4, d9 Q12 (hand-over half). Ledger
read in full: 502 lines, last entry l.493 "2026-11-14 — D9S-5b: a refused batched subscribe never delivers".
0a: `plans/reviews/D9S-6-audit-0a.md` (CLEAR; requires a CLOSING cell and Q12's origin settled).*

## Summary
L18's one site goes into the server build's batch thunk. When turning a window into its C form fails,
the binding still receives that window, once, as a no-batch delivery. The window's rows are added to its
loss count, and domain 7's reason is kept, CLOSING included. The empty `catch (...)` and its expired
"lost uncounted" comment are deleted. A test-only server shim, never packaged, carries the fault hook.
**Default (Decision 1 (a)): no failure message or origin crosses**, so Q12's hand-over half resolves to
"none". No new public surface.

## Design
**1. Two phases, one call** (the thunk, `batches_server.cpp:100-140`). Today one `try` holds both the
hand-over and `on_batch`, and swallows everything. It becomes:
```cpp
const int32_t reason = ReasonOf(status.reason);                 // noexcept (step 3)
const int64_t offered = batch != nullptr ? batch->num_rows() : 0;
/* sets, set_ptrs, array, schema, ReleaseUnmoved guard: declared here, as today */
fl_batch out = {};
try {                                                           // THE HAND-OVER: everything or nothing
    /* today's steps unchanged */  FLETCHER_HANDOVER_FAULT_POINT();   // empty outside the test shim
} catch (...) {                                                 // d9 L18, BC-12: the one site
    out = fl_batch{reason, status.rows_dropped + offered, 0, nullptr, nullptr, nullptr};
}
try { on_batch(ctx, &out); } catch (...) {}                     // step 2
```
The fallback is **written fresh** from two values fixed before the attempt; it copies no pointer from the
attempt. Whatever the attempt built is released after the call by the existing guard and the frame,
exactly as an unmoved batch is (BC-13 unchanged). `on_batch` is called **exactly once per window**, outside
the hand-over's `try`, so no window is ever delivered twice; for CLOSING this keeps ruling :478 ("exactly
one closing delivery"). The sum is L18's own wording ("the batch's rows counted as dropped in that window's
loss count"). It is arithmetic on one delivery and keeps no state, so L15/BC-9's "no count" holds and the
source scan is unaffected.

**2. The callback's own throw** breaches `h:1042` and d9 L1, but only after the window was handed over, so
nothing is lost. The guard keeps L18's "never throws back into domain 7" true; d7 L10 would swallow it anyway.

**3. `ReasonOf` becomes `noexcept`.** The exhaustive `switch` and the warning-as-error stay, so a fourth
reason still fails the build (DEBT-2). The unreachable tail changes from `throw` to `std::abort()`: a reason
is the one thing the fallback cannot invent, and a window may never cross as reason 0 (`h:1004`).

**4. The fault hook (D-D9S-4): test-only, and absent from every shipped artefact.**
- `c-abi/tests/handover_fault.cpp` (new): `#error` unless `FLETCHER_C_ABI_HANDOVER_FAULT` is defined.
  Exports `fl_test_handover_fault_arm(int64_t nth)` (the nth hand-over from now fails, once) and
  `int64_t fl_test_handover_fault_fired(void)`, plus the internal point. The point throws `std::bad_alloc`
  after the export has succeeded, which is the realistic cause and the worst case for cleanup. Its state is
  atomics. These are `fl_test_*` exports like the probe shim's, not public surface.
- `batches_server.cpp`: under `#if defined(FLETCHER_C_ABI_HANDOVER_FAULT)` the point calls into that file;
  otherwise it is `((void)0)` (about 6 lines). `c-abi/tests/CMakeLists.txt`, server variant only: a SHARED `fletcher-c-abi-handover-fault`, built the
  way the probe-shim block builds its shim (`CMakeLists.txt:227-259`), from the shipped shim's four
  sources plus `handover_fault.cpp`. It gets the shipped shim's visibility, version script and variant
  SERVER; the switch error through `target_compile_options` (the parent directory's source property does
  not reach `tests/`); `pubsub-arrow`; and the macro. The executable `c_abi_handover_tests` compiles
  `test_batches.cpp` with `FLETCHER_EXPECTED_VARIANT=2` and `FLETCHER_HANDOVER_FAULT_TESTS=1`. It links
  only that shim, gtest, Arrow and the `pubsub-arrow` oracle, and uses `TEST_PREFIX "handover."`.
- **Why it cannot ship:** (i) the target exists only in `tests/`, which only `FLETCHER_BUILD_TESTS`
  configures; (ii) the macro and the hook source are on that target alone, and CMake compiles
  `batches_server.cpp` once per target, with that target's definitions; (iii) `package()` copies the shipped
  stem by name (`conanfile.py:163-179`), and no edge or server pattern matches `fletcher-c-abi-handover-fault`;
  (iv) `check_shim_exports.sh` compares each shipped build's `fl_` exports with `binding.h`, so a shipped
  `fl_test_handover_*` export fails CI by name.

**5. What crosses: Decision 1.**
- **Under (a), the default:** a failed hand-over is exactly the no-batch delivery that d7 L7 and d9 L14
  already define. It is also exactly what a C++ caller gets from domain 7's own finish failure (d7 L10: "an
  internal failure becomes a drop"; `BatchStatus` has no message). `binding.h`'s `fl_batch` comment gains
  "or the shim could not hand it over (d9 L18)". **Q12, hand-over half: no `fl_origin`, because no
  `fl_error` crosses.**
- **Under (b):** `fl_batch` gains a trailing `const fl_error* failure` (NULL unless a hand-over failed;
  borrowed for the call), filled by `Capture` with `FL_INTERNAL` and an appended `FL_ORIGIN_HANDOVER = 4`
  (still ABI 0.8: nothing has shipped since D9S-1). About +25 lines and 2 surface items; it changes a
  signature after rung 4, against the ladder's accumulation check, which the PM records.

## Corner cases forbidden
1. **One window delivered twice** (handed over, then the fallback too). Rung 1: one call site, after the `try`.
2. **A no-batch carrying a half-built array, schema or attachments.** Rung 1: the fallback is a fresh struct.
3. **The reason lost, or CLOSING lost.** Rung 1: `reason` is fixed before the attempt; a fourth enumerator
   is a build error. An out-of-range value at run time means corruption, and `abort()` beats delivering 0.
4. **The loss count overflowing.** Rung 1, by d7 L2: the sum is the window's rows, already an `int64_t`.
5. **A hand-over throw reaching domain 7; a retry; a partial batch.** Rung 2: one catch, no rethrow, one
   attempt, all or nothing.
6. **The hook in a shipped artefact.** Rung 1: the build graph (4 (i)-(iii)); (iv) is the machine check.

**Handled:** a callback that throws (step 2). *Why not forbidden:* the shim cannot make foreign code
non-throwing (L1: "the shim cannot enforce it"); terminating instead is new behaviour no ruling grants.

## Premises and stop conditions
- **P1** CMake compiles a source once per target, with that target's definitions. If the fault shim could
  only share the shipped shim's object: **STOP-AND-ASK**, because the hook would ship.
- **P2** `package()` takes the shipped stem only. If any pattern matches the fault shim, rename the target
  before anything else. If no name can escape the patterns: **STOP-AND-ASK**.
- **P3** Domain 7 calls the thunk once per window, in order, on its batch thread (d7 L12), and a return counts
  as delivered. If it retries or inspects the return: **STOP**, because L18's "to it, the window was
  delivered" is false.
- **P4** Each TEST runs in its own process (`gtest_discover_tests`), and the arm counts from its own call. **P5** With `max_rows` 2 and `INT64_MAX`, 5 rows give RL(2), RL(2), then CLOSING(1) at close (as
  `ClosingFromOutsideDeliversTheRestThenTheClosingBatch` shows). If the shape varies: stop and report, and
  never loosen the test.

## Forcing-test mapping
**Forcing test:** `handover.Batches.AWindowTheShimCannotHandOverStillArrivesCounted`, in `test_batches.cpp`
under `#if FLETCHER_HANDOVER_FAULT_TESTS`. It reuses `BatchFixture`, `PublishedRows`, `BatchSink` and
`CloseBatches`. Two cells, k = 2 and k = 3; each publishes 5 rows, arms hand-over k, and closes from outside.
Each cell asserts: exactly 3 deliveries, RL, RL, CLOSING, with exactly one CLOSING and it last; delivery k
has no array, schema or attachments, `num_rows` 0, and `rows_dropped` equal to its window's rows (2 or 1);
every other delivery equals what was published (`ExpectWindows`); Σ`num_rows` + Σ`rows_dropped` == 5;
nothing arrives after close returns (2 s); and `fired == 1`. That last check is internal and only guards
against a vacuous run; the proof itself is external.

**Red for the right reason (pre-change):** the hook, the artefact and the test land first, over today's
thunk. Delivery k is swallowed: k=2 sees 2 deliveries and Σ 3 ≠ 5, and k=3 sees no CLOSING. **Green:** step 1's
catch delivers the window.

**Mutations, each red:** M1, restore the empty catch: both cells. M2, drop `+ offered`: Σ and k's
`rows_dropped`. M3, a constant `FL_BATCH_ROW_LIMIT` in the fallback: k=3 has no CLOSING. M4, keep `out.array`
in the fallback: k's `had_array`. M5, build the fault shim without the macro: `fired == 0`. M6, put the macro
on the shipped target: a link error, or `check_shim_exports.sh` once the hook source is added as well.

**Controls:** **C1**, the whole `Batches` suite also runs unarmed in `c_abi_handover_tests` and stays green,
so the hook is inert and the artefact is the server build. **C2**, `c_abi_tests` stays green on edge and
server, so no edge or row answer changes (L12, L13).

## Risks / Unknowns
- **Decision 1 narrows rung 8's text.** It drops "the failure's message beside it" and "an appended
  `fl_origin`", which d9 L18's "Ambiguous in the ruling" note also reads in. Under (a), the PM annotates the
  ladder, and the L18 note and d9 §4's "Not yet built: `failure`" are rewritten. That is a plan-text change,
  and the owner's call: hence the Decision.
- **The half-built export's release is proven by structure (one guard), not measured.** The allocation
  baseline is still owed to the ASan/LSan leg (D9S-5a DEBT-1).
- **Also:** C1 adds about 20 s of server `Batches` time to CI. There is no coexistence window: the empty
  catch dies here and nothing later depends on it. Untouched: the edge build, domains 7 and 2, `dotnet/`,
  and every status number.

## Files-to-touch
`c-abi/src/batches_server.cpp`; `c-abi/include/fletcher/abi/binding.h` (the `fl_batch` comment);
`c-abi/tests/handover_fault.cpp` (new); `c-abi/tests/CMakeLists.txt`; `c-abi/tests/test_batches.cpp`.
Docs: `docs/domains/d9-c-abi-shim.md` (L18 `[built]`, §2 Hand-over, §4, Q12); `docs/domains/bounded-context-register.md`
(BC-12 `[built]`); `c-abi/README.md` where it describes the null batch.

## Files-to-delete
The thunk's empty `catch (...) {}` and its comment (`:103-106`, `:138-139`) → the L18 site (ruling 2026-11-13
(2)'s window ends). `ReasonOf`'s `throw` → `std::abort()`. Under (a), d9 §4's "Not yet built: `failure`" and
L18's message reading: no replacement, disclosed in the brief. Tests: none (0a found no test that asserts or accepts the gap).

## Lines
**Production +34 / −16** (`batches_server.cpp` +31/−15, `binding.h` +3/−1). **Test-only seam +75**
(`handover_fault.cpp` ~30, `tests/CMakeLists.txt` ~45). **Tests +110.** Stage about +219 / −16, within the
250 ceiling. Under (b), add about +25 production lines.
