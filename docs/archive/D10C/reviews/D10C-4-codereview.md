# D10C-4 code review (step 4b, full lane) — base ff86539, working tree

**Verdict: no blocking findings.** 0 blocking / 3 should-fix / 6 nit. No fallback path survives: `Resolve` never returns zero for `fletcher-c-abi`, nothing loops around `Load`, the by-name `TryLoad` is gone, and the BC-10 scan guards all three. `Lazy` with `ExecutionAndPublication` is thread-safe and caches the thrown exception, so a refusal stays a refusal. It holds one decision per ALC, as documented. `PackageRid` gives win-x64, linux-x64, linux-arm64, freebsd-x64 and osx-arm64, and every key outside the two shipped ones gets "ships no shim". The CI YAML is correct: both legs' `find`/`cp`/`test -f` fail on a missing copy, Git-bash `cp` of a Windows path has a precedent in the AOT step, `env -u LD_LIBRARY_PATH` is kept, and the ci.pr filter plus the sparse lists gain arrow-bridge and pubsub-arrow.

## Should-fix
- **S1 — the refusal message stops short now that presence decides (conf. med).** `LoadFromCandidates` catch, NativeLoader.cs ~L133. The refusal names the file but not which build it is. It does not say that a server file takes precedence over an edge file that is present, and it puts every refusal down to the platform floor. Under presence semantics, the likely new cause is a stray, corrupt or wrong-arch `fletcher-c-abi-server` beside an app that works. The reader should be told "server build preferred; remove it to run the edge build". The message also dropped `'fletcher-c-abi'`. Dropping the probed list on the refused path is fine, because the chosen file is what matters.
- **S2 — the unloadable row's error match is not alias-proof (conf. low-med).** `LoaderLayoutTests.cs:78` uses `e.Contains(server, Ordinal)` with the full `Path.GetTempPath()` path. `SameFile` was hardened against exactly this: GitHub's windows-2022 TEMP is `C:\Users\RUNNER~1\...`, and if the host canonicalises `Assembly.Location`, the row goes red on CI only. Locally the TEMP path has no 8.3 form. Match on `Path.Combine(Leaf, LoadedShim.ServerFileName)`, as `SameFile` does.
- **S3 — premise P1 is undisclosed for hosts that load from the NuGet cache (conf. med).** In .NET Interactive `#r "nuget:"`, dotnet-script and FSI, `Fletcher.Interop.dll` loads from `~/.nuget/packages/.../lib/<tfm>/`. The package's `runtimes/` folder sits at `../../` from there, so it is not a candidate, and the deleted by-name probe (via `NATIVE_DLL_SEARCH_DIRECTORIES`) was what found the shim. Those hosts now stop. That is P1's own STOP-AND-ASK case. Either the owner rules it out of scope, or `dotnet/README.md` names it next to PATH and single-file.

## Nits
- N1 — the no-shim message reads "...not the operating system's library path. Also probed: ..." with nothing earlier in the message that was probed; it should say "Searched:".
- N2 — `LoadedShim.IsShim` is a prefix match and also counts `fletcher-c-abi-probe`. Match the two exact names.
- N3 — no test covers `PackageRid(Linux, Arm64)` or an unsupported arch reaching "ships no shim". `ADistroBuiltRuntimeStillFindsTheLinuxX64Folder` re-asserts `CandidatePaths` and `PlatformFloor("linux-x64")`, which T42 and T43 already cover. Trim it to the `PackageRid` asserts and add arm64.
- N4 — `CodeLines` skips any line that starts with `*`, so an unsafe dereference such as `*p = NativeLibrary.Load(...)` is invisible to the BC-10 scan.
- N5 — if the `StagedApp` constructor fails partway through the copy, it leaves a partial temp folder, because `Dispose` never runs.
- N6 — the baked `bin/$(Configuration)/$(TargetFramework)/` path breaks under `--artifacts-path` or a custom `OutDir`. It fails loudly, and the pattern has a precedent in NativePeer.

## Linux paths not verified by reading (none has run)
- `Process.Modules` reads `/proc/<pid>/maps` and lists only file mappings with r-x. A dlopen'd `.so` has one, so `Count==1` holds. Paths are kernel-resolved, so the full-path compares in `TheLoadedShimIsTheServer...` and conformance `LoadedShimTests` assume `/workspace`, the test output and `mktemp` are not symlinked.
- The G1 arm on Linux (`LD_LIBRARY_PATH` set on the child; red-first was shown on Windows/PATH only).
- That the dlopen "invalid ELF header" message still carries the path (the resolver adds it, so this is likely).
- P2 on Linux: the server `.so` loads with no siblings. ci.dotnet does not unset `LD_LIBRARY_PATH` and does not run `check_shim_dependencies.sh` on the server shim. Only conformance runs `env -u`.
- That CMake names the file `libfletcher-c-abi-server.so`, which the `test -f` checks hard-code.
- The full Fletcher suite on the Linux server build, the net8.0 roll-forward of the TestHost, and `DOTNET_HOST_PATH` inside the container.

## Test bulk
The +797 is mostly doc comments and the process harness, not padding. N3 is the one trimmable test. `OnlyTheFirstPresentFileIsTried` is a cheap in-process leg of the outside proof in `AnUnloadableServerShimStopsTheAppAndLoadsNoShim`, and is worth keeping.

## RECORD
- RECORD: design §Design says "`NativeFileNames` decorates either base name"; the code has `NativeFileName(string)`.
