# D10C-10 code review (step 4b)

Scope: `git diff -- dotnet .github docs` plus the untracked `dotnet/tests/Fletcher.Handover.Tests/`; design `plans/D10C-10-handover.md`.
Verdict: **0 blocking, 0 should-fix, 3 nits.** The design landed as briefed. Nothing in it was due for deletion, and nothing was left in place that should have gone.

## Checked and clean
- **Ownership.** `ToException` reads `in FlError` and disposes nothing. `ThrowIfFailed` disposes exactly once, in its `finally`; `captured?.Throw()` and `throw ToException(...)` both run inside that `try`. The thunk passes `in *(FlError*)source->Failure`: a reference to the borrowed struct, which is neither copied nor disposed. The shim's `fl_error_dispose(&failure)` runs after `on_batch` (`batches_server.cpp`), so there is no double free.
- **Message.** It is decoded by length. A null pointer or a zero length gives `NoMessage`. A length over `int.MaxValue` throws `OverflowException` from the `checked` cast before any byte is read. In the thunk the local `try` contains that throw, so the window still arrives with `Failure == null`, and the leg `ACopiedFailure...` turns red without that `try`. In `ThrowIfFailed` the overflow is loud and the `finally` still disposes (unchanged behaviour).
- **Thread safety.** The decode runs on the batch thread, inside the callback, while the borrow is valid. It touches no shared state and takes no lock. Only a managed `string` and two enums leave the callback.
- **`Errors.NoMessage` via reflection.** `GetRawConstantValue` on an internal `const` is stable. If the field is renamed or removed, the read throws `InvalidOperationException` (loud, not vacuous). `FletcherException.Message` is the raw `base(message)`, so the `NotEqual` comparison is meaningful.
- **CI, Linux.** Inside the single-quoted `bash -c`, the new lines contain no apostrophes. An empty `FAULT` gives `echo | grep -c .` = 0, which fails by name, and a `find` on a missing directory fails under `set -e`. `$FAULT` is still in scope at `dotnet build`.
- **CI, Windows.** The output follows the `fault=` pattern of the other outputs. CMake makes no POST_BUILD or `TARGET_RUNTIME_DLLS` copy of the fault shim, so "exactly one" holds on a clean checkout. `conan build` honours `tools.build:skip_test`. Gtest and Arrow come from `--build=missing` or the cache.
- **Test robustness.**
  - The fault state is two process-wide atomics. `DisableTestParallelization` serialises the assembly. Each TFM runs in its own host process, so state is not shared.
  - Each test arms after subscribing, and subscribing makes no hand-over. `arm(n)` zeroes `fired`, so `fired == 1` / `== 0` reflects only this test.
  - The idle Closing is a hand-over too: `FLETCHER_HANDOVER_FAULT_POINT` sits after the `if (batch)`. So `arm(2)` with 9 rows means four hand-overs, and `IndexOf == nth-1` is exact.
  - Delivery is synchronous: publish, close, then snapshot, as in the D10C-7 precedent `SubscriberArrowTests:1081-1087`. No timing is involved.
- **Vacuity.** The forcing test is guarded by `fired == 1`, by the constructor's `File.Exists` (it fails, never skips), and by `fl_test_*` imports that cannot resolve against a shipped shim. The legs use a pair no hard-coding would produce (`PayloadTooLarge`, `Seam`), a byte-exact multi-byte message followed by bytes that are not part of it, and an in-place zeroing predicate. I found no vacuous path.

## Not verifiable by reading (Linux-only unless marked)
1. The `conan build` of c-abi with `variant=server` and `run_tests=True` in the gcc13 container.
2. That `find` gets the `.so` under the single-config `build/Release/...` layout.
3. That `exports.map` lets `fl_test_*` through.
4. That glibc `dlopen` deduplicates the test's `LibraryImport("fletcher-c-abi-server")` with the loader's absolute-path load (design P3), so the single-copy marker does not fire.
5. That libstdc++'s `bad_alloc::what()` is non-empty.
6. *(Windows)* That MSYS converts the POSIX-form `$PWD/c-abi/build/...` path inside `-p:FletcherHandoverFaultShim=`. `locate_shim.sh` (`$HOME`-based) already relies on the same conversion.

## Nits
- N1: The source scan's `Contains("finally")` and `Contains("throw ToException(")` are not tied to their position. Any `finally` anywhere in `Errors.cs` satisfies them. (high confidence)
- N2: `PreserveNewest` plus the `Exists()` condition means that locally, a fault shim staged by an earlier build satisfies `File.Exists` when the property is later absent or wrong. CI is unaffected. (medium)
- N3: The forcing test does not `arm(0)` at its end. If it fails before the fault fires, the shim stays armed. Today's tests are harmless, since each re-arms before publishing, but a disarm in `Dispose` would make that independent of test order. (low)

## RECORD
None. The comments, the README line, d10 marks, D26 and BC-12 match the tree.
