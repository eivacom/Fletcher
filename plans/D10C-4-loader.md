# D10C-4 — the loader picks the shim: server when present, loud when broken, never both

Round D10C, rung 4 of `plans/D10C-test-ladder.md`, tree `b317201`. Citations as the ladder (`d10`, `rul`, `reg`;
`NL` = `dotnet/src/Fletcher.Interop/NativeLoader.cs`; `aud0a` = `plans/reviews/D10C-4-audit-0a.md`).
Rulings read top to bottom: **603 lines, 45 entries, last `rul:595`**. No tier-1 collision: `rul:575 (1)` names and
revises `rul:363 (4)`; `rul:536 (2)` retires `rul:525` by its own terms; d10 L5, D21, D28 and d9 L17 agree.

## Summary

`NativeLoader.Resolve` becomes the one site deciding which shim file loads (d10 L5; BC-10's deferred half). Presence
decides: a server file in any of the app's folders wins, else the edge file. The first file present is loaded once; if
it will not load, the app stops naming the file and the OS's reason, and nothing else is tried. Name-based probing
(`NL:73`) goes. The dotnet CI lane runs the suite again with both files present. The conformance lane stages the server
file under its own name (`rul:525`'s copy retires).

## Design

**One decision per load context (rung 1).** `Resolve` still answers only for `NativeMethods.LibraryName`. It returns
a static `Lazy<IntPtr>` (`ExecutionAndPublication`, which caches a thrown exception), so every import gets the same
handle and a refusal stays a refusal. That holds per `AssemblyLoadContext`: a plugin host that loads
`Fletcher.Interop` twice gets two decisions, and if they differ, d9 L17's marker refuses the second shim loudly. The
`NativeLoader` comment says exactly this.

**The candidate list (pure, tested without loading).** `CandidatePaths(assemblyDir, appDir, rid)` keeps today's folder
order: assembly folder, then app folder, deduplicated, each flat and then `runtimes/<rid>/native/`. It yields that
order **twice**, first for the server file name and then for the edge one, so every server candidate precedes every
edge candidate and a server file anywhere beside the app wins (L5). The server base name `fletcher-c-abi-server`
(`rul:395`, d10 D11) is a private constant beside `LibraryName`; `NativeFileNames` decorates either base name.

**The folder key (review B1; P6).** `rid` becomes `PackageRid(OSPlatform, ProcessArchitecture)` = `<os>-<arch>`;
`win-x64` and `linux-x64` are D-BIND-27's two shipped RIDs. It is no longer `RuntimeInformation.RuntimeIdentifier`,
which is `ubuntu.24.04-x64` on a distro-built .NET 8+, while a NuGet app's shim sits in `runtimes/linux-x64/native/`.
The same key feeds `PlatformFloor` and the RID messages, which also fixes "ships no shim" being reported on Ubuntu.
**Why this option and not the host's `NATIVE_DLL_SEARCH_DIRECTORIES`:** that list is a second folder source, computed
by the host from `deps.json` and possibly pointing into the package cache. Presence would then depend on host state
rather than on the app's folders. The key keeps one site and one folder set (BC-10, L5).

**First present, loaded once (rung 1: no fallback loop).** `LoadFromCandidates(candidates, rid)` keeps its signature.
It walks the list with `File.Exists` only, picks the **first existing** path, and calls `NativeLibrary.Load` on that one
path. There is no loop around `Load` (today's continues past a refused candidate, `NL:119-126`), so "edge in place of a
present server" and "another copy after a refused one" cannot be expressed.
If nothing is present, today's RID message stands (D-BIND-172; T43 `:224` keeps passing). If the file is present but
refused (rung 2), a `DllNotFoundException` in today's wording ("found its native shim … could not load it: `<path>`:
`<OS reason>`", then the floor) names the chosen **file** and adds that no other copy is tried (D28; T43 `:193` passes).

**The runtime's default probing is deleted** (`NL:70-76`; BC-10 EXTEND-AND-NARROW). `TryLoad` by name cannot see a file
that is present but broken, so it can never honour D28. The folders it would search for a packaged app are already
candidates. What goes is the OS search path (PATH, LD_LIBRARY_PATH, system folders), settled by `rul:575 (1)` ("one
resolver site, no fallback branch"), d10 L5/D28 ("its default probing included") and BC-10; the brief discloses it.

**The variant stays internal** (d10 L6 leaves it to the round; surface 0). The external proof is the OS module list;
`fl_binding_variant`'s one production call is rung 7's; rung 2's row stays the proof of `FlVariant`'s numbers (L2).

**CI.**
- **`ci.dotnet.yml`, both OSes:** add `arrow-bridge` and `pubsub-arrow` to the sparse checkout, to `ci.pr.yml`'s filter
  and to the `conan create` loop, then `conan create c-abi -o '&:variant=server'`. Find `SERVER` with `locate_shim.sh
  fletcher-c-abi-server variant=server`; build with `-p:FletcherServerShim=$SERVER`. The edge leg runs as today. The
  server leg comes after it: copy `$SERVER`, under its own name, into every `Fletcher.Tests` TFM output folder (fail if
  there is none), then run `dotnet test tests/Fletcher.Tests --no-build`.
- **Conformance lane** (T47): the server pass builds with `-p:FletcherNativeShim=$SERVER`, whose `Link` keeps the file's
  own name. The renaming `cp` and every `cmp` line go. Linux keeps `env -u LD_LIBRARY_PATH`, so `conanrun.sh` cannot
  hide a P2 breach. Add `test -f "$OUT/<server file>"` and the observation row below.
- **`Fletcher.Tests.csproj`:** bakes `AssemblyMetadata` for the server shim path, the host's output folder and
  `dotnet/src` (the protoc-dotnet pattern; CI path maps rewrite `[CallerFilePath]`). Adds a
  `ReferenceOutputAssembly="false"` reference to the host.

**The test host** (new project `dotnet/tests/Fletcher.TestHost`, reused at rung 11). An exe on the public API only:
both TFMs, `RollForward=LatestMajor`, no shim staged. It publishes one row on `inprocess`, receives it through
`SubscriberArrow.Subscribe` and prints `delivered`. In a `finally` it prints `module <file>` for each loaded
`fletcher-c-abi*` / `libfletcher-c-abi*` module. On an exception it prints `error <type>: <message>` for each exception
in the chain and exits 3. The module filter is one helper, `dotnet/tests/Shared/LoadedShim.cs`, shared with
`Fletcher.Tests` and the conformance project.

## Corner cases forbidden

| Case | Rung | How |
|---|---|---|
| Edge loaded while a server file is present | 1 | all server candidates first; only the first present is loaded |
| Fallback past a refused file (server→edge, or edge copy→edge copy) | 1 | no loop around `Load`; the refusal names the file and the OS reason |
| A different choice later in one load context | 1 | one latched decision per `AssemblyLoadContext`; across contexts, d9 L17's marker refuses a second shim |
| A shim reachable only through PATH / LD_LIBRARY_PATH / system folders | 2 | not a candidate; the no-shim message lists the folders searched (disclosed; `rul:575 (1)`, L5/D28, BC-10) |
| Native loading outside the resolver | 2 | BC-10 scan: no `NativeLibrary.` outside `NativeLoader.cs`; no `LibraryImport(`/`DllImport(` outside `dotnet/src/Fletcher.Interop` |

The edge→edge narrowing is new: two edge copies, the first broken, used to load the second; now the app stops. That is
licensed by `rul:575 (1)`'s "no fallback branch" and disclosed. **Handled residue:** with no assembly file location
(NativeAOT, single-file) only the app folder is probed. That is today's code, and NativeAOT is supported (D-BIND-81).
An OS or architecture outside D-BIND-27 still gets an `<os>-<arch>` key, which the "ships no shim" message names.

## Premises and stop conditions

- **P1.** Every supported layout puts the shims in the assembly or app folder, flat or in `runtimes/<rid>/native/`.
  **STOP-AND-ASK** if one depends on the OS search path or on `deps.json` probing into the package cache.
- **P2.** The server shim loads with no sibling native file. **STOP** if it needs Arrow DLLs beside it (rung 5's Q7).
- **P3.** File name ⇔ variant, by d9's per-target constant (BC-10). The loader trusts the name (L3).
- **P4.** `Process.Modules` lists loaded native libraries by file name on Windows and Linux (.NET 8/10). **STOP** if not:
  the external proof would then need a public variant (owner).
- **P5.** All of `Fletcher.Tests` is green on the server shim (rung 3). **STOP** on a row red only there; never skip it.
- **P6.** A package's native folder is the portable `<os>-<arch>` RID, whatever RID the runtime was built for
  (D-BIND-27). **STOP** if a shipped RID breaks that shape (e.g. a musl folder of its own).

## Forcing-test mapping

The rung tests (`Fletcher.Tests`' `LoaderLayoutTests`) run the host from staged temp folders (external). The edge
file comes from beside the test assembly, the server file from the baked path; with none baked they **fail**.

| Test | Green by | Red at `b317201`, for the right reason |
|---|---|---|
| `TheServerShimLoadsWhenPresentBesideTheApp`, theory: edge ∈ {flat, runtimes} × server ∈ {flat, runtimes} | server-first list, one load | the host prints `module <edge file>` (`NL:73`) |
| `AnUnloadableServerShimStopsTheAppAndLoadsNoShim`: **control** (`aud0a`), {valid edge, server = non-library bytes} | first present, no loop, loud refusal | exit 0 with the edge loaded: the forbidden fallback |
| `AnAppWithOnlyTheEdgeShimRunsOnIt` | unchanged | **guard**, green both ways (L7) |
| ci.dotnet server leg + `NativeLoaderTests.TheLoadedShimIsTheServerExactlyWhenItIsBesideTheTests` (leg: one module; variant `Server` ⇔ a server file is beside) | resolver; CI staging | the edge is loaded beside a server file (`aud0a`) |
| Conformance server pass + `TheShimBesideTheTestsIsTheOneLoaded` (both passes, same rule, by module) | own-name staging | a server-only folder finds no shim |
| T42 rewritten (`aud0a`): every server candidate before every edge candidate, in folder order; counts 8/4/4 | candidate list | 4 candidates, edge only |
| leg `OnlyTheFirstPresentFileIsTried`: [refused A, refused B] names A, never B | no loop | HEAD lists both |
| leg `ADistroBuiltRuntimeStillFindsTheLinuxX64Folder`: `PackageRid(Linux, X64)` = `linux-x64`; candidates hold `runtimes/linux-x64/native/`; the floor names Linux-gcc13 | folder key (B1) | HEAD keys on the runtime's RID. A leg only: CI's runtimes are portable, so no outside run sees a distro RID |
| guard `NoNativeLoadOutsideTheResolver` (BC-10 scan of the baked `dotnet/src`) | — | green at HEAD. Control: it fails as vacuous unless it finds `NativeLoader.cs`'s own `NativeLibrary.Load` |
| T43 (`:44-:239`), unchanged, on both legs (L1 handshake rows included) | — | kept |

## Risks / Unknowns

- **CI time:** the dotnet lane now builds Arrow C++ on both OSes. That is the end state: rung 5 packs this shim from it.
- **The wrapper:** a refusal reaches the app as a `TypeInitializationException` around the named `DllNotFoundException`,
  as every load failure does today. Tests assert the inner exception.
- **Stale docs out of scope:** d9 L13 "Tests" and §6 (read-only here) describe the renamed copy. Reported for d9's
  next ingestion.
- **Coexistence windows:** none (d10 D16 annotated retired). **STOP-AND-ASK:** none. **Owner decision:** none.

## Files-to-touch

- **Code and CI:** `dotnet/src/Fletcher.Interop/NativeLoader.cs`;
  `.github/workflows/{ci.dotnet.yml, ci.pr.yml, ci.integration-test.binding-abi-conformance.yml}`; `dotnet/Fletcher.slnx`.
- **Tests:** `dotnet/tests/Fletcher.Tests/{Fletcher.Tests.csproj, NativeLoaderTests.cs, LoaderLayoutTests.cs (new)}`; new
  `dotnet/tests/Fletcher.TestHost/{csproj, Program.cs, packages.lock.json}`; new `dotnet/tests/Shared/LoadedShim.cs`;
  `integration-tests/binding-abi-conformance/dotnet/BindingAbiConformance/{csproj, one test file}`.
- **Docs (listed, not counted):** `NL:4-14, 58-87` (F14); `dotnet/README.md:147-158` (F15); conformance `README.md:77-92`;
  the full-suite commands gain `-p:FletcherServerShim=` (`README.md:324`, `docs/architecture-overview.md:354`,
  `dotnet/README.md:83`); `ci.dotnet.yml:115-117, :253-255` ("arrow-bridge is NOT in the loop"); d10 L5/D16/§4 marks;
  register BC-10 status.

**Lines (counted production):** `NativeLoader.cs` code +30/−18, CI YAML +60/−30, csproj/slnx +35: **≈ +125 / −48, net
≈ +77** (ceiling 250). Not counted: `NativeLoader.cs` doc comments ≈ ±20; tests ≈ +295/−10.

## Files-to-delete

- **`NL:70-76`** (`TryLoad` by the edge name), **`NL:111-127`** (the continue-past-refused loop) and **`NL:85`**'s
  `RuntimeIdentifier` key: replaced by the one candidate decision, "first present, loaded once", and `PackageRid`.
- **Conformance `:119-127`, `:211-235`:** the renaming `cp`, both `cmp` checks and the edge-folder control. Replaced by
  own-name staging, a presence check and `TheShimBesideTheTestsIsTheOneLoaded` (`env -u` stays; header `:42-55` rewritten).
- **T42's count assertions** (`:166, :173, :176`): **re-anchored**. The subject (folder order) is lawful; "the server file
  is never a candidate" is forbidden by L5, so it is rewritten. **No test is deleted;** T47 is re-anchored (d9 L13).
