# D10C-2 — Stage Brief (2026-11-15)

**In one sentence:** every number the native library hands a C# app (failure status, failure origin, batch reason, build variant) has the same value in C# as in the C header; "could not hand over a batch" gets a C# name, and the batch reasons take the header's numbers.
**Forcing test:** `AbiNumberTableTests.EveryNumberTheShimHandsOutHasTheSameValueInCSharp` reads the C header and requires each table's names and numbers to equal C#'s, with no extras on either side.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Failure origin "hand-over" (4) | NEW public member | In the header (d9 D32); C# had no name for it (d10 L2, D26, ruling :551 (3)) |
| Batch reasons: row limit, timeout, closing | CHANGED: 0-2 become 1-3 | Equal to C (L2, D34). Visible: a cast or stored number shifts by one; accepted because batches are not on `main` (D4) |
| "Not supported" and "seam" descriptions | CHANGED (doc text only) | "This provider or this build cannot do this" (D28); seam includes the library's own refusal (h:265). Closes F17 |

The build-variant mirror is internal, so no new public type is added.

## Deleted
Nothing. The stage only adds C# names for numbers the header already publishes and corrects the reason numbers.

## Corner cases forbidden vs handled
**Forbidden:** an unknown number (origin 5, reason 0 or 4, variant 3) never reaches C#, because the ABI handshake refuses any library of another version (d10 L1). C# therefore gets no range check, which would re-answer a lower layer (L3). The table test catches drift at every later stage, and a missing header fails the build instead of being skipped.
**Handled:** `default(BatchReason)` is now 0, which has no name. C# enums always admit their default, so this cannot be forbidden; D34 accepts it, and the binding never produces it.

## How
- **Mechanism:**
  - `FletcherOrigin` gains `Handover = 4`; `BatchReason` becomes `RowLimit = 1`, `Timeout = 2`, `Closing = 3`.
  - An internal `FlVariant { Edge = 1, Server = 2 }` and the `fl_binding_variant` import go into `NativeMethods.cs`, for rungs 4 and 7.
  - `Errors.ThrowIfFailed` is unchanged: its existing cast yields `Handover` once the member exists.
- **Production files (3, about 25 touched lines):** `dotnet/src/Fletcher/FletcherException.cs` (+3, plus doc lines at FE:38 and FE:67), `dotnet/src/Fletcher/SubscriberArrow.cs` (3 changed, SA:116-126), `dotnet/src/Fletcher.Interop/NativeMethods.cs` (about +14; keeping the enum beside its import keeps the stage at 3 files).
- **Tests (not counted):** a new `dotnet/tests/Fletcher.Tests/AbiNumberTableTests.cs` beside T44 (`ErrorTests`, kept), and a 3-line copy item in `Fletcher.Tests.csproj`.
- **The table test** has four rows: `fl_status`, `fl_origin`, `#define FL_BATCH_*` and `fl_variant`, against `FletcherStatus`, `FletcherOrigin`, `BatchReason` and `FlVariant`. It strips each header prefix, converts the name to PascalCase, and requires the two sets of name and number to be equal.
- **Finding the header:**
  - It reads the repo's `c-abi/include/fletcher/abi/binding.h`, copied into the test output by a `<None Include … CopyToOutputDirectory>` item, the csproj's existing way of reading goldens (:95-106).
  - In CI, both test jobs check out `c-abi` (`ci.dotnet.yml:91`, `:234`). The pack job does not, but it builds no test project.
- **Red before the change:**
  - The origin row fails on 4, and the reason row fails on all three members.
  - The variant row fails with the assertion "no C# mirror", because the type is looked up by name.
  - The status row passes both ways and is a declared guard.
- **Legs:** `ErrorTests.OriginFourIsHandover` (origin 4 maps to a defined member named "Handover"; today it is the unnamed "4"), and `fl_binding_variant` on the staged shim answering a defined member.
- **Verify, from `dotnet/`:** `dotnet build Fletcher.slnx -c Release -p:FletcherNativeShim=<edge shim from scripts/locate_shim.sh fletcher-c-abi variant=edge> && dotnet test Fletcher.slnx -c Release --no-build`
- **Docs (listed, not counted):**
  - FE:38 and FE:57-75 (F17).
  - d10: the L2 mark and Tests line, §4 :395-396, :425 and :436, Q11 closed, and Findings (a)2 and (c) first bullet.

## Decisions for you
None. The stop checks are clear:
- The only new public surface is `Handover`, which d10 names (L2, D26).
- The variant mirror is internal, as L2 places it; whether to make it public is rung 4's call (d10 L6).
- No C# check re-answers a lower layer.
- No d9 file is edited; the header is only read.

## Risks accepted / debt carried
- While `FlVariant` stays internal, the `fl_variant` row is the **sole** proof that its numbers match. Rung 4 only observes the variant, rung 7 proves batches rather than numbers, and C# refuses nothing on the variant (L6). This is lawful because nothing outside depends on the numbers yet (0b audit). If rung 4 makes the variant public or decides on it, rung 4 re-proves it from outside.
- `fl_binding_variant` has no production caller until rung 4, a gap of one or two stages; nothing routes through it.

## Numbers
Declared net lines: production about +20 / −5 in 3 files; tests about +95 in 3 files. New public surface: 1. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
