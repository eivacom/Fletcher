# D10C-2 — Stage Brief (2026-11-15)

**In one sentence:** every number the native library hands a C# app (a failure's status, where it came from, why a batch closed, which build is loaded) means the same in C# as in the C header, so the failure "the library could not hand over a batch" has a name in C#, and the three batch reasons are numbered as the C header numbers them.
**Forcing test:** `AbiNumberTableTests.EveryNumberTheShimHandsOutHasTheSameValueInCSharp`. It reads the C header and checks that every status, origin, batch reason and build variant has a C# member with the same name and number, and that C# has no extra member in either direction.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Failure origin "hand-over" (4) | NEW public member | The C header has it (d9 D32). C# had no name for it (d10 L2, D26, ruling :551 (3)) |
| Batch reasons: row limit, timeout, closing | CHANGED: 0-2 become 1-3 | They now equal the C values (L2, D34). A visible change: a cast or a stored number shifts by one. Accepted because batches are not on `main` (D4) |
| "Not supported" and "seam" descriptions | CHANGED (doc text only) | "Not supported" now reads "this provider or this build cannot do this" (D28). "Seam" now includes the library's own refusal (header :265). This closes F17 |

No new public type: the build-variant mirror is internal (see *How*). New public surface: 1 member.

## Deleted
Nothing. This rung only adds C# names for numbers the header already publishes, and corrects the reason numbers. Nothing is retired.

## Corner cases forbidden vs handled
**Forbidden:** a number C# does not know (origin 5, reason 0 or 4, variant 3) never reaches C#. The ABI handshake refuses any library whose version differs (d10 L1), so C# gets no range check of its own; adding one would re-answer a lower layer (L3). Silent drift in either direction is caught by the table test, which runs at every later stage. If the header is missing, the build fails; the test never quietly skips.
**Handled:** an uninitialised reason (`default`) is now 0, which has no name. This cannot be forbidden because C# enums always admit their default. D34 accepts it, and the binding never produces it.

## How
- **Mechanism.** The C# enums take the header's numbers.
  - `FletcherOrigin` gains `Handover = 4`.
  - `BatchReason` becomes `RowLimit = 1`, `Timeout = 2`, `Closing = 3`.
  - An internal `FlVariant { Edge = 1, Server = 2 }` and an internal `fl_binding_variant` import are added to `NativeMethods.cs`. Rungs 4 and 7 use them.
  - `Errors.ThrowIfFailed` is unchanged: once the member exists, its existing cast yields `Handover`.
- **Production files (3), about 25 touched lines:**
  - `dotnet/src/Fletcher/FletcherException.cs`: +3, plus 2 doc lines at FE:38 and FE:67.
  - `dotnet/src/Fletcher/SubscriberArrow.cs`: 3 changed, at SA:116-126.
  - `dotnet/src/Fletcher.Interop/NativeMethods.cs`: about +14. The enum sits beside its import, which keeps the stage at 3 files.
  - Declared net production: about +20 / −5. Within the light lane's limit of 60 lines and 3 files.
- **Tests (excluded from the count):**
  - A new `dotnet/tests/Fletcher.Tests/AbiNumberTableTests.cs`, about +90. It sits beside T44 (`ErrorTests`), which is kept unchanged.
  - About +3 in `Fletcher.Tests.csproj`.
- **The table test.** One row per table: `fl_status`↔`FletcherStatus`, `fl_origin`↔`FletcherOrigin`, `#define FL_BATCH_*`↔`BatchReason`, `fl_variant`↔`FlVariant`. Each header name is converted to PascalCase after its prefix is stripped, and the test requires the two sets of name and number to be equal.
- **How the test finds the header.** It reads the repo's `c-abi/include/fletcher/abi/binding.h`, not the package's copy and not the Conan cache's. A `<None Include="../../../c-abi/include/fletcher/abi/binding.h" CopyToOutputDirectory>` item copies it into the test output. That is the csproj's existing way of reading repo files (its goldens, csproj :95-106). In CI, both test jobs check out `c-abi` (`ci.dotnet.yml:91`, `:234`). The pack job does not check it out, but it builds no test project.
- **Red for the right reason, before the change:**
  - The origin row fails because header 4 has no C# member.
  - The reason row fails on all three members (0/1/2 against 1/2/3).
  - The variant row fails with "no C# mirror": the test finds the type by name, so the failure is an assertion, not a compile error.
  - The status row is green both ways. It is a declared guard.
- **Legs:**
  - `ErrorTests.OriginFourIsHandover`. `ThrowIfFailed` with origin 4 gives a defined member whose name is "Handover". Before the change it gives the unnamed value "4".
  - `fl_binding_variant` on the staged library answers a defined `FlVariant` member, which shows the import resolves.
- **Verify, from `dotnet/`:** `dotnet build Fletcher.slnx -c Release -p:FletcherNativeShim=<edge shim in the Conan cache, via scripts/locate_shim.sh fletcher-c-abi variant=edge> && dotnet test Fletcher.slnx -c Release --no-build`
- **Docs (listed, not counted):**
  - FE:38 and FE:57-75 (F17).
  - In d10:
    - L2's build-state mark becomes built, and its Tests line names the new test.
    - §4 :395-396, :425 and :436 are marked built.
    - Q11 is closed.
    - Findings (a)2 and (c), first bullet.

## Decisions for you
None. Checked and clear:
- **New public surface:** only `Handover`, which d10 names (L2, D26).
- **The variant mirror is internal**, as L2 places it ("NativeMethods and AbiTypes are a mechanical mirror"). Whether C# exposes the loaded build publicly is left to the round's design (d10 L6), so it belongs to rung 4.
- **No C# check re-answers a lower layer.**
- **No d9 edit:** the header is only read.

## Risks accepted / debt carried
- The variant row checks an internal mirror, not a public one. It is one row of the table: the variant's behaviour is proven from outside at rung 4 (which build loads) and rung 7 (batches only on the server build). If rung 4 makes the variant public, that is rung 4's decision.
- `fl_binding_variant` is imported but has no production caller until rung 4. That gap lasts one or two stages, and nothing routes through it.

## Numbers
Declared net lines: production about +20 / −5, in 3 files. Tests about +95, in 3 files. New public surface: 1. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
