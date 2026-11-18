# D10C-10 — a failed hand-over reaches the C# handler in `BatchStatus` (rung 11)

Round D10C, item D10C-10, branch `feature/csharp-switch`, HEAD `55b890d`. Ledger `plans/RIX-rulings.md` read in full:
**667 lines**, last heading `## 2026-11-16 — D10C-7: the switch and the deletion are one stage` (:660). Binds: :383 (1),
:512, :551 (3); d10 L2, L3, L8, L9, L14, D15, D26; d9 L2, L18, D30, D32; register BC-12 (C# half); audit `D10C-10-audit-0a` (a)-(d).

## Summary
When the server shim cannot hand a window over, the C# batch handler gets that window: a null batch, its rows in
`RowsDropped`, its reason, and the shim's failure (status, origin, message) in `BatchStatus`. One new public member,
no new type. Proven from outside by d9's test-only hand-over fault shim, staged under the server file name in a test
project of its own (the CopyOracle pattern). No d7/d9 file is edited.

## Design
1. **`BatchStatus.Failure`** (`FletcherException?`, get-only; the one new public member). Null on every window
   except one the shim reports it could not hand over. d10 names it (§4 "the failure in `BatchStatus`", L9, D26);
   the ladder leaves its shape to the architect. `FletcherException` already carries exactly {`Status`, `Origin`,
   `Message`}; a new type would duplicate it. The internal constructor gains an optional `failure` argument, so the
   per-row site (`SA:485`) is untouched.
2. **One decoder, two callers.** `Errors` splits into `ToException(int status, in FlError err, string? sentence)`
   (builds the exception, the D-BIND-54 `FletcherFormatException` rule included, and disposes nothing) and
   `ThrowIfFailed` (rule 3, `throw ToException(...)`, `fl_error_dispose` in its `finally`, as today). The failure in
   a `BatchStatus` is therefore the same object `ThrowIfFailed` would have thrown for that `fl_error`, and the
   message is copied by length (rule 1), never by scanning.
3. **The thunk's branch** (`BatchDelivery.Deliver`, before the status is built): if `source->Failure != 0`, then
   `failure = Errors.ToException(f->Status, in *f)`. Status and origin are copied **as given**: no hard-coded
   `Internal`/`Handover`, and no check that a failure comes with a null array (d10 L3: the shim's answer, not C#'s).
   Never `fl_error_dispose`: the failure is borrowed for the call (d10 L8, d9 L2) and the shim frees it after the
   callback returns (`batches_server.cpp:169`). The rest of the thunk does not change: a null array passes through,
   `RowsDropped` stays as given, and a failed Closing still frees the `GCHandle`, because the reason is read from
   the struct.
4. **Safety.** Only managed data (a `string` and two enums) leaves the callback. The decode runs on domain 7's batch
   thread inside the thunk's existing containment (d10 L10). It takes no lock.
5. **CI and local staging of the fault shim** (no d9 edit). The fault shim is built only by c-abi's test build of
   the server variant (`tests/CMakeLists.txt:217-252`), and no Conan package carries it. In both `ci.dotnet.yml`
   jobs, after the existing server `conan create`, run
   `(cd c-abi && conan build . --build=missing -pr:a=$PROFILE -o "&:variant=server" -o "&:run_tests=True" -c tools.build:skip_test=True)`.
   Then find **exactly one** `*fletcher-c-abi-handover-fault.{dll,so}` under `c-abi/build` (zero or several fails
   the lane by name) and pass it to `dotnet build` as `-p:FletcherHandoverFaultShim=<path>`. The implementer may
   narrow this to the one CMake target, within the declared lines. Locally it is the same command, and
   `dotnet/README.md` gains one line naming it. The new project `dotnet/tests/Fletcher.Handover.Tests` stages the
   file as `fletcher-c-abi-server` (a `Link` that replaces `-handover-fault` with `-server`). `NativeLoader` then
   loads it as the server shim (d10 L5). If the property is absent, the tests **fail**, naming it. They never skip,
   as in CopyOracle.

## Corner cases forbidden
- **C# invents a failure** (rung 1). `Failure` is set only from a non-NULL `fl_batch.failure`. The thunk's own
  take-over failure (D10C-7 residue (a)) stays as it is unless the owner chooses otherwise (brief Decision 1).
- **C# re-answers status or origin** (rung 1). They are copied as given. The leg `TheBatchThunkForwardsAFailureAsGiven`
  feeds an unusual pair (`PayloadTooLarge`, `Seam`) and turns red if either is hard-coded.
- **Disposing or keeping the borrowed failure** (rung 1). `ToException` takes `in FlError` and disposes nothing. The
  only disposer in `Errors` is `ThrowIfFailed`, which the thunk never calls. What the handler gets is a managed copy.
- **A failure with a non-null array, or status `FL_OK`.** Forbidden by d9's contract (`h` `fl_batch.failure`).
  C# forwards whatever arrives and does not re-check (L3).
- **Handled residue: copying the failure throws** (out of memory while building the string or the exception). The
  window is still delivered, rows counted and reason kept, with `Failure` null. *Why not forbidden:* an allocation
  failure cannot be made unrepresentable, and L9's "the window arrives" outranks "with its why". Its test is the
  leg `ACopiedFailureThatCannotBeDecodedStillDeliversTheWindow` (review DEBT-3).

## Premises and stop conditions
- P1: the fault shim exports `fl_test_handover_fault_arm`/`_fired` on both OSes (`FL_ABI_EXPORT`; `exports.map`
  `fl_*`), answers `FL_VARIANT_SERVER` and the same ABI version. **STOP-AND-ASK** if the loader or the handshake
  refuses it: no loader change to admit a test artefact without the owner.
- P2: `conan build` of c-abi with `variant=server, run_tests=True` builds the fault shim with no recipe or CMake
  edit. **STOP-AND-ASK** if it needs one (d9 production).
- P3: the test assembly's `[LibraryImport("fletcher-c-abi-server")]` resolves to the module the resolver already
  loaded (same file, same folder), so the process holds one shim (d9 L17). **STOP-AND-ASK** if the single-copy
  refusal fires. The fallback would need `InternalsVisibleTo`, which :636 left alone.
- P4: the fault counter is process-wide, so the project runs its tests sequentially (one class, parallelisation off).

## Forcing-test mapping
The external test is `Fletcher.Handover.Tests.HandoverTests.AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure`,
a Theory with two cells. Setup: `inprocess`, `MaxRows = 3`, `Timeout = InfiniteTimeSpan`, arm after subscribing,
then publish and `Unsubscribe`. Unsubscribe from outside the handler returns after the Closing, so the test needs
no timing.
- **(A) `arm(2)`, 9 rows.** Window 2 (row limit) fails, windows 1 and 3 arrive, then the idle Closing.
- **(B) `arm(3)`, 7 rows.** The Closing window (1 row) fails, and `Unsubscribe` still returns.

Each cell asserts all of the following. `fl_test_handover_fault_fired() == 1`, so a run with no fault cannot pass.
Exactly one delivery has a non-null `Failure`. On that delivery the batch is null, `RowsDropped` equals the
window's rows, the reason is `RowLimit` (A) or `Closing` (B), `Status == Internal` and `Origin == Handover`. Its
`Message` is non-empty and is not the binding's no-message fallback. The audit (c) forbids a literal, because
`bad_alloc::what()` differs by runtime. Every other window has `Failure == null`. Rows offered = Σ batch rows
+ Σ `RowsDropped`.
- **Red for the right reason:** today the project does not compile, because `BatchStatus` has no failure. Step-3
  evidence: add the member returning null before the thunk branch, and every cell goes red on `Failure != null`
  with `fired == 1`.
- **Control (guard):** `UnarmedNoWindowCarriesAFailure`, the same publish with no arm: `fired == 0`, no failure,
  and conservation holds. It is green both ways and declared.
- **Legs** (`Fletcher.Tests/SubscriberArrowTests`, which already drive `BatchThunk` by reflection, `:2061`):
  - `TheBatchThunkForwardsAFailureAsGiven`: a crafted `FlError` whose message holds multi-byte UTF-8 and no NUL
    arrives byte-exact, with its status and origin. This is the "unchanged" proof the external test cannot spell.
  - `TheBatchThunkNeverDisposesTheBorrowedFailure`, for audit (a) and L8. The test gets a shim-allocated `FlError`
    from a refused call (as `HandleTests:115`), passes it as `failure`, and asserts that after the thunk returns
    the struct is unchanged (`fl_error_dispose` zeroes it, `binding.cpp:263-273`). The test then disposes it.
    Its control: an **in-place** dispose of the borrowed struct turns the predicate red. A dispose of a struct
    **copy** (`var e = *f; fl_error_dispose(ref e)`) frees the buffer but leaves `*f` intact, so it is **outside
    this leg** (the shim would then double-free at `batches_server.cpp:169`). That case is covered by the optional
    one-line scan that `SubscriberArrow.cs` references no `fl_error_dispose` (true today) (review DEBT-1).
  - `ACopiedFailureThatCannotBeDecodedStillDeliversTheWindow` (review DEBT-3; the handled residue). A crafted
    failure with `MessageLen > int.MaxValue` makes the `checked` cast at `Errors.cs:92` throw deterministically.
    The leg asserts exactly one delivery: a null batch, rows counted, reason kept, `Failure == null`. It is red if
    the decode's `try` is missing (the window would be swallowed by the thunk's outer catch, `SA:559-562`).
  *Why a leg:* if C# did dispose the failure, nobody outside could see it, because the shim's own later dispose
  is then a no-op.

## Risks / Unknowns
- **Edge leg not applicable.** A hand-over exists only on the server build, and the edge refusal of
  `SubscribeBatched` is rung 7's tested truth table. Declared, not skipped.
- **CI cost.** Both jobs gain a c-abi server build with its tests compiled (several minutes each). Narrowing to the
  one target is allowed.
- **Locally**, `dotnet test Fletcher.slnx` without the property fails this project, as CopyOracle's already does.
  `dotnet/README.md:85-88` gains the property.
- **Evidence.** Until a green run on the owner's draft PR, the marks read "Windows, by hand; CI and Linux unrun"
  (ledger :408 (2)).
- **Decision 1** (C#'s own take-over failure) confirms the spec: d10 L9 (:287-288) already records (a), and no
  ruling covers the case. Under the default (a), the leg `:2061` gains `Assert.Null(status.Failure)`. (b1) reuses
  origin `Handover` and strains L2. (b2) widens `Failure` to `Exception?`, so handlers must type-test to read status
  and origin; L2 holds. Neither has an external trigger (out of memory), so only a leg would prove it. Disclosed.
- No coexistence window. Nothing later in the ladder touches this site.
- **Size:** about +90 / −4 counted lines in 5 files, which exceeds the light lane (60 lines, 3 files).

## Files-to-touch
`dotnet/src/Fletcher/SubscriberArrow.cs` (`BatchStatus.Failure` and its constructor argument, the thunk branch,
header and `RecordBatchHandler` doc) · `dotnet/src/Fletcher/Errors.cs` (`ToException` split) ·
`dotnet/tests/Fletcher.Handover.Tests/{Fletcher.Handover.Tests.csproj, HandoverTests.cs, packages.lock.json}` (new)
· `dotnet/Fletcher.slnx` · `.github/workflows/ci.dotnet.yml` (both jobs) ·
`dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` (three legs, `:2061`'s assertion under Decision 1) ·
`ErrorTests.cs:104-105` (stale remark, prose). Docs, listed and not counted: `dotnet/README.md`, d10 L8, L9, L14
marks and §4, register BC-12 C# half.

## Files-to-delete
None. The rung adds a branch to a thunk that already delivers the null batch (D10C-7). The audit found no test that
asserts a forbidden behaviour in scope (`D10C-10-audit-0a`: CLEAR), and no construct is superseded.
