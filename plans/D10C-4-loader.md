# D10C-4 — the loader picks the shim: server when present, loud when broken, never both

Round D10C, rung 4 of `plans/D10C-test-ladder.md`, tree `b317201`. Citations as the ladder (`d10`, `rul`, `reg`;
`NL` = `dotnet/src/Fletcher.Interop/NativeLoader.cs`; `aud0a` = `plans/reviews/D10C-4-audit-0a.md`).
Rulings read top to bottom: **603 lines, 45 entries, last `rul:595`**. No tier-1 collision: `rul:575 (1)` names and
revises `rul:363 (4)`; `rul:536 (2)` retires `rul:525` by its own terms; d10 L5, D21, D28 and d9 L17 agree.

## Summary

`NativeLoader.Resolve` becomes the one site deciding which shim file a process loads (d10 L5; BC-10's deferred
half). Presence decides: a server file in any of the app's folders wins, else the edge file. The first file present
is loaded once; if it will not load, the app stops with an exception naming it and the OS's reason, and nothing else
is tried. The runtime's name-based probing (`NL:73`) goes. CI's dotnet lane builds the server shim and runs the
suite a second time with both files present; the conformance lane stages the server file under its own name, so
`rul:525`'s renamed copy retires.

## Design

**One decision per process (rung 1).** `Resolve` still answers only for `NativeMethods.LibraryName`, and returns a
process-wide `Lazy<IntPtr>` (`ExecutionAndPublication`, which caches a thrown exception). Every import gets the same
handle and a refusal stays a refusal, so "both shims" cannot happen through C# (d9 L17's marker remains the backstop).

**The candidate list (pure, tested without loading).** `CandidatePaths(assemblyDir, appDir, rid)` keeps today's
directory order (assembly folder, then app folder, deduplicated; each flat, then `runtimes/<rid>/native/`) and
yields it **twice**: first for the server file name, then for the edge file name. `NativeFileNames` takes the base
name and adds the platform decoration. The server base name `fletcher-c-abi-server` (`rul:395`, d10 D11) is a
private constant beside `LibraryName`. Every server candidate precedes every edge candidate: a server file anywhere
beside the app wins (L5).

**First present, loaded once (rung 1: no fallback loop).** `LoadFromCandidates(candidates, rid)` keeps its signature
but walks the list with `File.Exists` only, picks the **first existing** path, and calls `NativeLibrary.Load` on that
one path. With no loop around `Load` (today's continues past a refused candidate, `NL:119-126`), neither "edge in
place of a present server" nor "another copy after a refused one" can be expressed.
- Nothing present: today's RID message, unchanged (D-BIND-172; T43 `:224` keeps passing).
- Present but refused (rung 2): `DllNotFoundException` in today's wording ("found its native shim … the operating
  system could not load it: `<path>`: `<OS reason>`", then the floor). It names the chosen **file**, not
  `LibraryName`, and adds one clause: no other copy is tried (D28). T43 `:193` keeps passing.

**The runtime's default probing is deleted** (`NL:70-76`; BC-10 EXTEND-AND-NARROW). `TryLoad` by the edge name
loads by name and cannot see a file that is present but broken, so it can never honour D28. The folders it would
search for a packaged app (the app folder and its `runtimes/<rid>/native/`) are already candidates. What goes is the
OS search path (PATH, LD_LIBRARY_PATH, system folders): **Brief Decision 1**, recommendation (a).

**The variant stays internal.** d10 L6 leaves a public variant property to the round; none is added (surface 0).
The external proof observes which file is loaded through the OS module list. `fl_binding_variant` gets its one
production call at rung 7, and rung 2's table row stays the proof of `FlVariant`'s numbers (d10 L2).

**CI.**
- `ci.dotnet.yml`, both OSes:
  - add `arrow-bridge` and `pubsub-arrow` to the sparse checkout, to `ci.pr.yml`'s dotnet filter and to the
    `conan create` loop, then `conan create c-abi -o '&:variant=server'`;
  - set `SERVER=$(locate_shim.sh fletcher-c-abi-server variant=server)` and build with `-p:FletcherServerShim=$SERVER`;
  - **edge leg:** as today;
  - **server leg:** afterwards, copy `$SERVER` under its own name into every `Fletcher.Tests` TFM output folder
    (fail if there are none), then `dotnet test tests/Fletcher.Tests --no-build`.
- Conformance lane (T47 re-anchored, `aud0a` (b)): the server pass builds with `-p:FletcherNativeShim=$SERVER`,
  whose `Link` keeps the file's own name. The renaming `cp` and every `cmp` line on both OSes are deleted. They are
  replaced by `test -f "$OUT/<server file>"` and the observation row below.
- `Fletcher.Tests.csproj` bakes three `AssemblyMetadata` values: the `FletcherServerShim` path, the test host's
  output folder and `dotnet/src`. It uses the protoc-dotnet pattern, not `[CallerFilePath]`, which CI path maps
  rewrite. It also takes a `ReferenceOutputAssembly="false"` reference to the host.

**The test host** (new test project `dotnet/tests/Fletcher.TestHost`; rung 11 reuses it) is an exe on the public
API only: both TFMs, `RollForward=LatestMajor`, no shim staged. It creates an `inprocess` provider, declares a
topic, subscribes per row through `SubscriberArrow.Subscribe`, publishes one row, waits up to 10 s and prints
`delivered`. In a `finally` it prints `module <file>` for every loaded module named `fletcher-c-abi*` or
`libfletcher-c-abi*`. On an exception it prints `error <type>: <message>` for each exception in the chain and exits
3. The module filter is one shared helper, `dotnet/tests/Shared/LoadedShim.cs`, compiled by the host,
`Fletcher.Tests` and the conformance project.

## Corner cases forbidden

| Case | Rung | How |
|---|---|---|
| Edge loaded while a server file is present | 1 | all server candidates first; only the first present is loaded |
| Fallback past a refused file (server→edge, or one edge copy→another) | 1 | no loop around `Load`; the refusal names the file and the OS reason |
| Two shims, or a different choice later in the process | 1 | one latched decision per process (`Lazy`, exceptions cached); d9 L17 backstop |
| A shim reachable only through PATH / LD_LIBRARY_PATH / system folders | 2 | not a candidate; the no-shim message lists the folders searched (Decision 1) |
| Native loading outside the resolver | 2 | BC-10 scan: no `NativeLibrary.` outside `NativeLoader.cs`; no `LibraryImport(`/`DllImport(` outside `dotnet/src/Fletcher.Interop` |

The edge→edge narrowing is new. Two edge copies in different folders with the first broken used to load the
second; now the app stops. This follows L5 ("decided at one site from what is present") and is disclosed in the
brief. **Handled residue:** an assembly with no file location (NativeAOT, single-file) probes the app folder only.
That is today's code, and not forbidden because NativeAOT is a supported consumer (D-BIND-81).

## Premises and stop conditions

- **P1.** Every supported layout puts the shims in the assembly or app folder, flat or under `runtimes/<rid>/native/`
  (NuGet framework-dependent build, RID-specific publish, NativeAOT, plugin host). **STOP-AND-ASK** if a supported
  deployment depends on the OS search path or on `deps.json` probing into the package cache.
- **P2.** The server shim loads with no sibling native file; the conformance server pass copies it alone today.
  **STOP** if a leg needs Arrow DLLs beside it: that is rung 5's Q7 breach and goes to the owner.
- **P3.** File name ⇔ variant, by d9's per-target constant (BC-10); the loader trusts the name, checks no variant (L3).
- **P4.** `Process.Modules` lists loaded native libraries by file name on Windows and Linux (.NET 8/10). **STOP** if
  not: the external proof would then need a public variant, which is an owner question.
- **P5.** All of `Fletcher.Tests` is green on the server shim (rung 3's claim). **STOP** on any row red only on the
  server leg; report it, never skip it.

## Forcing-test mapping

The rung tests run the host from staged temp folders (external) and live in `Fletcher.Tests`' `LoaderLayoutTests`.
The edge file comes from beside the test assembly and the server file from the baked path; with no path baked, the
tests **fail** and name the property.

| Test | Green by | Red at `b317201`, for the right reason |
|---|---|---|
| `TheServerShimLoadsWhenPresentBesideTheApp`, theory: edge ∈ {flat, runtimes} × server ∈ {flat, runtimes} | server-first list, one load | the host prints `module <edge file>` (`NL:73`) |
| `AnUnloadableServerShimStopsTheAppAndLoadsNoShim`: **control** (`aud0a` (d)), {valid edge, server = non-library bytes} | first present, no loop, loud refusal | exit 0 with the edge loaded: the forbidden fallback |
| `AnAppWithOnlyTheEdgeShimRunsOnIt` | unchanged | **guard**, green both ways (L7) |
| ci.dotnet server leg + `NativeLoaderTests.TheLoadedShimIsTheServerExactlyWhenItIsBesideTheTests` (leg: one module; variant `Server` ⇔ a server file is beside) | resolver; CI staging | edge loaded beside a server file (`aud0a` (c)) |
| Conformance server pass + `TheShimBesideTheTestsIsTheOneLoaded` (both passes, same rule, by module) | own-name staging | a server-only folder finds no shim |
| T42 rewritten (`aud0a` (a)): every server candidate before every edge one, in directory order; counts 8/4/4 | candidate list | 4 candidates, edge only |
| leg `OnlyTheFirstPresentFileIsTried`: [refused A, refused B] names A, never B | no loop | HEAD lists both |
| guard `NoNativeLoadOutsideTheResolver` (BC-10 scan of the baked `dotnet/src`) | — | green at HEAD (`aud0a` (e)). Control: it fails as vacuous unless it finds `NativeLoader.cs`'s own `NativeLibrary.Load` |
| T43 (`:44-:239`), unchanged, on both legs (L1 handshake rows included) | — | kept |

## Risks / Unknowns

- **CI time:** the dotnet lane now builds Arrow C++ on both OSes, as the conformance lane does. That is the end
  state: rung 5 packs this server shim from this lane.
- **The wrapper:** the resolver runs in `NativeMethods`' type initializer, so a refusal reaches the app as a
  `TypeInitializationException` around the named `DllNotFoundException`, as every load failure does today. Tests
  assert the inner exception. Changing that shape is another subject; not done.
- **d9 text, read-only here:** d9 L13 "Tests" and §6 describe the renamed copy (`rul:525`). It goes stale; this is
  reported for d9's next ingestion, not edited.
- **Coexistence windows:** none (the copy and `NL:73` go here; d10 D16 annotated retired). **STOP-AND-ASK:** none
  open; Decision 1 is the owner's, with a default grounded in the law.

## Files-to-touch

`dotnet/src/Fletcher.Interop/NativeLoader.cs` · `.github/workflows/{ci.dotnet.yml, ci.pr.yml,
ci.integration-test.binding-abi-conformance.yml}` · `dotnet/tests/Fletcher.Tests/{Fletcher.Tests.csproj,
NativeLoaderTests.cs, LoaderLayoutTests.cs (new)}` · `dotnet/Fletcher.slnx` · new
`dotnet/tests/Fletcher.TestHost/{csproj, Program.cs, packages.lock.json}` · new `dotnet/tests/Shared/LoadedShim.cs` ·
`integration-tests/binding-abi-conformance/dotnet/BindingAbiConformance/{csproj, one test file}`.
Docs (listed, not counted): `NL:4-14, 58-87` (F14), `dotnet/README.md:147-158` (F15), conformance
`README.md:77-92`, d10 L5 / D16 / §4 marks, register BC-10 status.

**Lines (counted production):** `NativeLoader.cs` code +25/−15; CI YAML +55/−24; csproj and slnx +33. **≈ +113 / −39,
net ≈ +74** (ceiling 250). Not counted: `NativeLoader.cs` doc comments (about ±20); tests (about +285/−10).

## Files-to-delete

- **`NL:70-76`** (`TryLoad` by the edge name): replaced by the one candidate decision.
- **The continue-past-refused loop** (`NL:111-127`): replaced by "first present, loaded once".
- **Conformance `:119-127` and `:211-235`:** the renaming `cp`, both `cmp` checks and the edge-folder control.
  Replaced by own-name staging, a presence check and `TheShimBesideTheTestsIsTheOneLoaded`; header `:42-55` rewritten.
- **T42's count assertions** (`:166, :173, :176`): **re-anchored**. The subject (directory order) is lawful; "the
  server file is never a candidate" is forbidden by L5, so the assertion is rewritten, not kept.
- **No test is deleted.** T47's subject (C# over the server build, d9 L13) is lawful and is re-anchored.
