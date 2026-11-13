# D9S-1 — Stage Brief (2026-11-13)

**Light lane: fits.** About 10 touched non-comment production lines (28 counting comments) in 3 production files.

**In one sentence:** a program can ask the loaded shim which build it is, at any moment and from any thread. Today's shim and the test-only probe shim both truthfully answer "edge". The shim's version moves to 0.8, and C# still loads it.
**Forcing test:** `BindingAbi.TheVariantIsAnsweredBeforeAnyOtherCall` and `BindingAbi.TheVariantAndVersionAnswerAlikeFromManyThreads`, run against both the shipped shim and the probe shim. Together they prove that the first call a program makes can be this question, that eight threads asking at once all get "edge", and that they get the bumped version.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| `fl_binding_variant()`, exported by every build | NEW | Answers the owner's "the program can ask the shim which build it has" (d9 L16, D23) |
| `fl_variant`: `FL_VARIANT_EDGE = 1`, `FL_VARIANT_SERVER = 2` | NEW | One vocabulary for both builds, so the server build (D9S-2) adds only its own answer and no new declaration |
| `FL_BINDING_ABI_VERSION_MINOR` 7 → 8; C#'s expected minor 7 → 8 | CHANGED | L6 (a change is never silent). D27 and D-D9S-6: one bump for the whole round, made at its first header addition, in the same commit as the one permitted C# line |

## Deleted
Nothing. This stage only adds; the round has nothing earlier to retire.

## Corner cases forbidden vs handled
**Forbidden** (by construction): the question failing, needing set-up first, or being refused. It returns a constant from plain C, touches no state and has no error path. It answers even in a shim that refuses to work because a second copy is loaded, just as the version does (L5; rung 9). An answer of 0 is not a build, so a zeroed variable or a wrongly declared import never reads as "edge". A second place that decides the build cannot arise: one function answers, and the later edge refusals call it (BC-10). Telling the builds apart by version is ruled out because both share one version (D27). An unknown future value never reaches a binding before 1.0: a binding built for another minor refuses the shim at the handshake (L6).
**Handled:** none.

## How   (the design)
- `binding.h`: bump the minor to 8. After `fl_binding_abi_version`, append the `fl_variant` enum and `FL_ABI_EXPORT fl_variant fl_binding_variant(void);`. Its comment says: callable first, from any thread, still answers in a poisoned shim, values appended only.
- `c-abi/src/binding_version.c` (already compiled into both the shipped shim and the probe shim): `fl_variant fl_binding_variant(void) { return FL_VARIANT_EDGE; }`. This is **BC-10's single site**. Its comment records that D9S-2 turns the literal into a per-target constant set in `c-abi/CMakeLists.txt`, and that the edge refusals from D9S-4 call this function. It is plain C, outside the containment wrapper, like the version.
- `dotnet/src/Fletcher.Interop/NativeLoader.cs:34`: `HeaderVersionMinor = 8`. This is the one domain-10 line (D27), in the same commit.
- No change to `exports.map` (`fl_*` already covers the new function), to CMake production files, or to `ci.c-abi.yml` (its export grep is prefix-only).
- Tests, not production: two rows in `c-abi/tests/test_binding_abi.cpp` (the first-call row, and 8 threads released together that compare variant and version). `tests/CMakeLists.txt` also compiles that file into `c_abi_probe_tests` and adds `TEST_PREFIX "probe."` there, so the ctest names stay unique. `test_package/src/example.c` asks for and prints the build, and fails on an unknown one. `c99_probe.c` compiles the new declaration as strict C without any edit.
- **Red for the right reason:** before the change the rows do not compile, because the query does not exist. **Control:** with the header bumped but the C# line left at 7, the dotnet lane's `NativeLoaderTests` refuse the shim. The implementer shows this once. The version rows stay green because they compare against the header (audit 0a: lawful).
- Docs: d9 L16 → `[built]` (edge and probe; server half D9S-2); L6's D27 bullet; §1 edge bullet; §2 "Variant query"; §4 "ABI 0.8, 48 entry points" and the "Both builds" bullet. README: file map, "The surface", "Consuming" step 2, test_package line. BC-10 status.

## Decisions for you
None. The query's name, its answer type and declaring the server value now are implementation choices. I picked the simplest; the reasons are above.

## Risks accepted / debt carried
- Until D9S-2, the "edge" answer is a literal at the single site, not yet the per-build constant the register describes. D9S-2 changes that one line and adds the server value. This is the ladder's plan; there is no second path.
- C# cannot ask the question yet. Declaring it in C# is domain 10's work (the C# switch round). C# behaves exactly as it does today.
- `ci.c-abi.yml:25` still says the ABI "exports one function" (audit 0a). I left it out to stay at 3 production files, and hand it to D9S-2, which edits that workflow anyway.
- Probe-shim ctest rows are renamed with a `probe.` prefix. No CI filter depends on those names (checked).

## Numbers
Declared net lines: **+85 / −21**, counted as the raw diff will count them: production +25/−3 (`binding.h` +17/−1, `binding_version.c` +7/−1, `NativeLoader.cs` +1/−1); tests +42/−4; docs +18/−14.
Touched production: 28 lines (about 10 non-comment) in 3 files. New public surface: 2 (one type, one function). Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
