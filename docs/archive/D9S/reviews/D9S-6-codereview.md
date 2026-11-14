# D9S-6 code review (step 4b): d43eb0b..abc0776

**Verdict: APPROVE.** 0 blocking, 0 should-fix, 4 nits. Read-only review; nothing was built.

## Focus areas (all hold)
- **on_batch exactly once.** It is called unconditionally after the try/catch. Nothing before the
  `try` throws (noexcept `ReasonOf`, an inline `num_rows()`, empty optionals, `= {}` aggregates). The
  catch cannot throw: `Capture` is noexcept, and the `fl_batch` assignment is trivial. The optionals
  rightly cover MSVC debug containers that allocate when default-constructed.
- **Loss count.** `rows_dropped + offered` is correct. `offered` is 0 when domain 7 gave no batch,
  whose rows domain 7 already counts (h:1029), and a batch's rows are disjoint from `rows_dropped`.
- **Failure lifetime.** Borrowed, and disposed after the call on every path, including success
  (`delete[] nullptr`) and a throwing `on_batch`. A callback that wrongly disposes it causes no double
  free: `fl_error_dispose` nulls the message.
- **Nothing half-exported.** The fallback rebuilds `out` with NULL `array`/`schema`, so the callback
  cannot take them. `ReleaseUnmoved` frees what the export built, at scope exit after `on_batch`.
- **DEBT-5.** No leak and no double free. `unique_ptr` guards the block in flight. The catch deletes
  the pushed blocks (refs==1, none handed out). A constructor that throws runs no destructor, and
  `push_back` after `reserve` does not throw.
- **abort.** Reachable only by an out-of-range enum value from domain 7. A missing enumerator fails
  the build on both shims (`/we4062`, `-Werror=switch`; the test shim sets it per directory).
- **Seam isolation.** The macro is set per target only, and CMake compiles the source once per
  target. `handover_fault.cpp` has a `#error` guard. The conan patterns `*{stem}.dll` and
  `*lib{stem}.so*` do not match `-handover-fault`. `fl_test_*` is exported by the test shim only.
- **Flakiness.** None found. It uses the `inprocess` provider and the same publish-then-close pattern
  as test_batches.cpp:740. The ~4 s is two 2 s waits that check nothing arrives after close; such a
  wait can only false-pass. `arm` resets `fired` between the two cells.

## Nits
- [high] The fault point sits only after the export, so the real failure sites (building the sets, a
  failed export) and the DEBT-5 catch are never reached by a test. Hoisting `sets.emplace()` above the
  `try` would pass.
- [high] There is no cell where domain 7 gives no batch and the hand-over fails, which is the case
  that checks `offered == 0`.
- [med] The positional `fl_batch{reason, …, &failure}` (batches_server.cpp:160) should use C++20
  designated initialisers.
- [med] DEBT-5 could be one mechanism: `push_back(new BlobOwner)` (reserved), then set `keep`, so the
  catch is the only guard.

RECORD: batches_server.cpp thunk comment says that before the `try` there are only "`noexcept` calls"; `num_rows()` is non-throwing but not declared noexcept.
