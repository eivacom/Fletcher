# D9S-7 — Stage Brief (2026-11-14)

**In one sentence:** a process that loads both shim builds, in either order, is shown to get one working shim: the later one refuses every call that can fail and names both files, yet still says which build and version it is. CI also proves the C# conformance suite passes on the server build as it does on the edge build.
**Forcing tests:** rung 9 (L5, L16, L17, BC-15): `TwoBuilds.TheLaterBuildRefusesNamingBothAndTheFirstKeepsWorking` and `TwoBuilds.ARefusedBuildStillAnswersItsVariantAndVersion`, each run in both load orders. Rung 10 (L13, ruling :516): the conformance lane's new server pass, every C# row green on the server shim.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| none shipped | none | Both builds already export the one-copy marker from one source, so nothing a user loads changes. CI only: the conformance lane gains a server pass. |

## Deleted
- The conformance lane's two "first file with this name in the cache" lookups. They are replaced by the lookup by package identity (D9S-2 DEBT-1).
- Nothing else. The decoy rows and the faked-refusal rows stay (audit 0a: lawful, not re-anchored).

## Corner cases forbidden vs handled
**Forbidden** (unrepresentable, or refused typed at the door):
- **The server pass quietly re-testing the edge file**, e.g. a stale copy left over from the edge pass. Each pass builds into its own empty folder, and the lane fails unless the shim beside the C# tests is byte-identical to the server file.
- **CI picking the wrong build when both are in one cache.** The build is chosen by its package's recorded options, and the lookup refuses when it finds none or several.
- **A "second copy" that is really the same build loaded twice, or a load that failed.** The test fails unless the loaded file answers to the other build.
- **Arrow reaching the edge build through this proof.** Rung 9 lives only in the server package's tests; the edge package's dependency graph is untouched, test-only or not.
- **The test-only edge copy shipping.** It is built only when tests are on, under a name that no packaging pattern matches, as the hand-over fault shim is.
- **Freeing a refusal with the wrong copy, or unloading the refused copy mid-process.** Each message is freed by the copy that wrote it, and the loaded copy stays loaded until the process exits.
**Handled:** none.

## How
**Rung 9** (test-only; expected green on arrival, declared). Files: `c-abi/tests/CMakeLists.txt`, plus a new `c-abi/tests/test_two_builds.cpp`.
- In its server block, `CMakeLists.txt` builds an **edge copy** from the edge build's own sources and settings (`FL_VARIANT_EDGE`, `batches_edge.cpp`, the same visibility and exports map). The file is `fletcher-c-abi-edge-twin`, placed beside the server shim.
- The same test source is built into two executables. `server-first` links the server shim and loads the edge copy by path. `edge-first` links the edge copy and loads the server shim by path. Each process loads the other build once, through the decoy's path helper.
- Test 1 checks that the loaded copy answers its own build, which must differ from the linked one. Its `fl_provider_create` must return `FL_INTERNAL` with both file names in the message, while the linked copy's still returns `FL_OK`.
- Test 2 checks the refused copy still answers: `fl_binding_variant` gives its build, `fl_binding_abi_version` equals the header's, and `fl_single_copy_marker` text equals the linked copy's (A32, so L13's list stays true).
- Mutations, each must turn a test red: raise the scan's threshold (no refusal); rename the marker export in one build (neither refuses, L17); run the check lazily at first call (first copy poisoned too); route the variant query through the refusal (Test 2).

**Rung 10** (ruling :516). Files: `ci.integration-test.binding-abi-conformance.yml` (both legs) and `ci.pr.yml`.
- Check out `pubsub-arrow` and `scripts`; create `pubsub-arrow` and a second c-abi with `variant=server`, no tests (the c-abi lane runs them). Both `find … -print -quit` lookups become `locate_shim.sh fletcher-c-abi variant=edge` (DEBT-1).
- After the edge pass, copy `locate_shim.sh fletcher-c-abi-server variant=server` to a scratch folder under the edge file name; `dotnet test -o <fresh folder> -p:FletcherNativeShim=<copy>`; then `cmp` that folder's shim with the server file. Control: staging the edge file instead turns `cmp` red.
- One corpus, emitted by the edge build, is round-tripped by C# through each build, so the server pass checks server bytes against edge bytes. The emitter recipe is unchanged; ladder rung 10's "passes the switch through" would compare the server build with itself. `ci.pr.yml`'s filter for the lane gains `pubsub-arrow/**` and `scripts/locate_shim.sh`. No `dotnet/` or csproj edit.

## Decisions for you
**None.** No stop trigger fires: no `dotnet/` or csproj edit (the rename is CI staging, ruling :516); no edge entry point changes its answer; Arrow never reaches the edge build; no new status number (the refusal is the existing internal-error status); no fault hook.

## Risks accepted / debt carried
- **Rung 9's edge half is a test-only rebuild from the edge build's sources, not the shipped edge file**, which sits in another package by identity. What L17 needs from it (the marker export, same source) is identical; recipe drift is caught only by Test 1's build assertion.
- **Finding, not fixed (Linux):** in edge-first, the refused server copy's calls to its own exported functions can bind to the edge copy's. Only the refused copy is affected, and it refuses before any body runs.
- **Docs owed (not counted):** d9 L5's test list, and its "still answer" bullet names the variant query (L16 already requires it); d9 L17 and BC-15 `[built]`; the ladder's rung 9 "Builds on" note (DEBT-1) and rung 10's emitter departure.

## Numbers
Declared net lines: **+30 / −8 (≈38 touched, 2 files)**, counting CI YAML as production by the ladder's rule; under "counts only if it ships", 0. Tests (not shipped, not counted): ≈+170 in 2 files. New public surface: 0. Design cycles used: 1/2.

---
*As landed:* (PM, at close)
