# D10C-10 — compliance review (step 4a)

## Cycle 1 — PASS: 0 blocking, 1 non-blocking (F1), 4 RECORD
**Proof of read:** `plans/RIX-rulings.md` read top to bottom, **684 lines, 54 `## ` entries**. The last is "## 2026-11-17 — Fletcher
Linking Exception: legal approval; pack it" (:677), which does not bind here. Binding rulings: :383 (1), :408 (2), :512, :525, :551 (3),
:636, :669. No tier-1 collision and no tier-2-vs-tier-1 conflict. :669 names :551 (3) and narrows nothing in it.
Scope: base `163d471` minus `4a8fb96` (the licence commit), so the uncommitted tree plus the untracked `Fletcher.Handover.Tests/`.

**Held (checked, not findings):**
- **Public surface.** `BatchStatus.Failure` (`FletcherException?`, get-only) is the only new public member. The constructor's
  `failure` argument, `ToException` and `NoMessage` are internal. d10 §4 names the member (L9, D26). The per-row site is unchanged.
- **One decoder, two callers (design 2).** `ToException(int, in FlError, string?)` builds the exception, applies D-BIND-54 and
  disposes nothing. `ThrowIfFailed` throws it, and its `finally` is the only `fl_error_dispose` in `Errors.cs` (:69).
- **Thunk branch (design 3).** The branch is `source->Failure != 0` alone, before the status is built. Status and origin are copied
  as given: no array or `FL_OK` check (L3), no `Internal`/`Handover` literal. The failure is never disposed or kept (L8). The
  decode's own `try` delivers the window with `Failure == null`. A Closing still frees the `GCHandle` (the reason is read from the struct).
- **:669.** Leg `:2061` asserts `Failure == null` for C#'s own take-over failure, and no code path invents a failure.
- **:551 (3) is proven from outside.** In both cells `fired == 1`, with exactly one failing delivery at index `nth-1`: a null
  batch, the window's rows counted, its reason kept, `Internal`/`Handover`, and a non-empty message that is not `NoMessage`. Every
  row is accounted for, and the one Closing comes last. The control's `arm(0)` disarms, as audit 0b accepted.
- **Legs match the design.** The as-given leg uses (PayloadTooLarge, Seam) and a byte-exact multi-byte message with trailing bytes.
  The never-disposes leg uses a shim-allocated `FlError`, an in-place predicate, and a scan for the struct-copy case (DEBT-1).
  DEBT-3's leg sets `MessageLen > int.MaxValue` and asserts one delivery. `D10C-evidence.md` records the red-first runs and the
  controls (in-place dispose, copy dispose, no `try`).
- **`Errors.NoMessage` by reflection** adds no `InternalsVisibleTo` and changes no csproj, so it complies with :636 (the precedent
  is `BatchThunk`).
- **No d7/d9 production edit.** `git status` shows nothing under `c-abi/`, `pubsub-arrow/` or `core/`. `c-abi/build` and
  `CMakeUserPresets.json` are gitignored. Only d9's existing test target is built (P2 held).
- **CI (design 5).** Both jobs run `conan build … variant=server run_tests=True skip_test` after the server `conan create`. Each then
  requires exactly one file: `grep -c .` != 1 fails by name, for zero and for several. The path reaches `dotnet build` as
  `-p:FletcherHandoverFaultShim`. The Linux script is single-quoted and the new lines add no apostrophe. The csproj follows the
  CopyOracle pattern, staged under `-server`: a missing shim fails and never skips. `DisableTestParallelization` is set (P4).
- **Files.** Files-to-touch match the diff exactly. Files-to-delete is "None", and nothing ordered gone survives in code. The stale
  remark at `ErrorTests.cs:104-105` has been rewritten.
- **Marks.** L8's and L9's new marks and the register's BC-12 C# half read "Windows, by hand; CI and Linux unrun". The evidence
  row lists Linux, NativeAOT and the CI YAML as unrun (:408 (2)). The exceptions are under RECORD.

### F1 (non-blocking): the dispose scan is narrower than the design's predicate (design :98, DEBT-1; audit 0b note 2)
The design asks for a scan showing that `SubscriberArrow.cs` "references no `fl_error_dispose`". The leg
`TheBatchThunkNeverDisposesTheBorrowedFailure` matches only the qualified `NativeMethods.fl_error_dispose`, so a
`using static … NativeMethods` call would pass it. The design marks the scan optional, so this does not block. The `Errors.cs`
half has the same weakness: its `Contains("finally")` is not tied to `ThrowIfFailed` (4b N1).
**Fix:** match the bare token `fl_error_dispose` in both files. Today the count is 0 in `SubscriberArrow.cs` and 1 in `Errors.cs`
(:69), so the assertions stay as they are.

### RECORD
- d10 marks without "Windows, by hand; CI and Linux unrun" (design Risks; :408 (2)): L14's new clause ("… (D10C-10, L9's
  tests)"), §4's "`[built]` (D10C-10)", D26's "Built: … (D10C-2, D10C-10)" and the Findings line "is tested (D10C-10, `HandoverTests`)".
- d10 L2 (:143): "`failure` included, unread until rung 11" is stale. Since D10C-10 the field is read.
- d10 :157 and :319 cite "`Errors.cs:44-75`". The tree now has `ThrowIfFailed` at :52-71 and `ToException` at :79-92.
- The thunk-branch comment in `SubscriberArrow.cs` and the doc of leg `ACopiedFailure…` cite ":669" for the copy-failure residue.
  :669 covers only C#'s own take-over failure. The copy case rests on the brief's "Handled" line and the design's residue,
  which d10 L9 already cites as "(D10C-10)".
