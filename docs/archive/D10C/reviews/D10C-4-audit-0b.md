# D10C-4 — step 0b test-authority audit (new-test) — **CLEAR**

**What I read.** I read `plans/RIX-rulings.md` top to bottom: **603 lines, 45 `## ` entries; the last is `:595` "2026-11-15 — D10C ladder: the domain-7 port tests"**. I checked the tests against `:575 (1)` (with the option text of pre-round audit R-1 (a), `:270-273`), `:536 (2)`, `:525`, `:395`, d10 L5 (D28), L6, L7, register BC-10 and ladder rung 4 (including its ground rules). I found no tier-1 disagreement.

**Scope.** All of it is uncommitted against `ff86539`: `LoaderLayoutTests.cs` (LLT), the `NativeLoaderTests.cs` diff (NLT), `Fletcher.TestHost/Program.cs`, `Shared/LoadedShim.cs`, the conformance `LoadedShimTests.cs`, and the csproj and slnx items.

| Test | Class | Note |
|---|---|---|
| `TheServerShimLoadsWhenPresentBesideTheApp` ×4 (LLT:40) | LAWFUL | Expects exactly one module, and it must be the staged server file. All four layout arms are covered, and the NuGet arms use the literal `runtimes/win-x64\|linux-x64/native`. It meets law (a) and law (b). |
| `AnUnloadableServerShimStopsTheAppAndLoadsNoShim` (LLT:64) | LAWFUL | Stages {a valid edge file, a server file that is not a library}. Expects no module loaded, exit code 3, and a `DllNotFoundException` naming the server file. This is the control the 0a audit asked for (watch (b)). |
| `AnAppWithOnlyTheEdgeShimRunsOnIt` ×2 (LLT:86) | LAWFUL | Asserts L7 and L5's "otherwise the edge shim". The module must be the staged file, so a copy found on PATH cannot pass it. |
| T42 `TheShimIsLookedForBesideTheAssemblyThenBesideTheApplication` (NLT:166) | LEGITIMATE-BUT-ADJACENT, re-anchored correctly | The 4-count that pinned "the server file is never a candidate" is gone. The test now asserts that every server candidate comes before every edge candidate. |
| `OnlyTheFirstPresentFileIsTried` (NLT:203) | LAWFUL | Its edge-to-edge subject rests on R-1 (a)'s chosen text "no fallback branch", not on L5's server-only sentence. The brief discloses it. |
| `ADistroBuiltRuntimeStillFindsTheLinuxX64Folder` (NLT:245) | LAWFUL (leg) | Reflection is acceptable while the test is RED. It fails closed: a missing method gives a named assert, never a pass. Change it to a direct call in the green commit, because the stated reason ("cannot compile") ends once `PackageRid` exists. |
| `TheLoadedShimIsTheServerExactlyWhenItIsBesideTheTests` (NLT:348) | LAWFUL (leg) | Checks the module list and the variant, not which files are present. It meets watch (a). |
| `NoNativeLoadOutsideTheResolver` (NLT:378) | LAWFUL (guard, RED for the right reason) | It is red on `NL:85-86` (`RuntimeIdentifier`) and on the missing `PackageRid(`. Its vacuity anchors `NativeLibrary.Load(` and `PackageRid(` are design commitments (D-28, B1), not accidents of today's code. |
| `TheShimBesideTheTestsIsTheOneLoaded` (conformance) | LAWFUL | Same rule on both passes, read from the module list. It re-anchors T47. |
| TestHost, `LoadedShim`, `StagedApp`, `HostRun` | LAWFUL | The host prints its `module` lines in a `finally`, so they appear on failure too. No shim is in the host's output, and that is asserted. |

**The questions you asked.**
- **Law (a):** no test asserts or tolerates a fallback, and every server leg checks which file loaded.
- **Law (b):** the LLT rows run the TestHost as a separate process and read its module list.
- **DEBT-5:** the outside proof now exists as tests: the literal-folder arms plus the scan. It becomes evidence only when the Linux leg of `ci.dotnet` runs green, so 4b must record that run ID.
- **Reflection:** acceptable while RED; switch to a direct call at green.
- **New VALUE subject:** none beyond rung 4. `PackageRid` is the mechanism behind rung 4's NuGet layout, the floor text is D-BIND-172 and is already pinned, and `FlVariant` is rung 2's.
- **Guards pinned to today's code:** none.

**Gaps (not blocking; they do not change the verdict).**
- **G1.** If nothing is beside the app, there is no outside proof that the OS search path is never probed.
  - Both no-shim rows call `LoadFromCandidates` directly, and the scan permits `TryLoad` inside `NativeLoader.cs`. So a `TryLoad`-by-name kept *after* the candidates would pass every row, which is the silent default probe that L5/D28 ("its default probing included") forbids.
  - Suggested fix: a host row with an empty staged folder and the edge file's folder on `LD_LIBRARY_PATH`/`PATH`, expecting no module and exit code 3. It is red at HEAD. Alternatively, make the scan fail on `TryLoad` in `NativeLoader.cs`.
- **G2.** The two "server beside the tests" checks (NLT:348 and the conformance test) fall back silently to asserting the edge if CI stages no server file. Step 4a must see `ci.dotnet` fail on a missing copy (design: "fail if there is none") and the conformance lane's `test -f`.
- **G3 (optional).** The scan could also match `RUNTIME_IDENTIFIER` (the `AppContext.GetData` route).
- **G4 (note).** BC-10's "no managed refusal keyed on the variant" scan is missing. It belongs to rung 7, not this rung.
