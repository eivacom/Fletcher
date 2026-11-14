# D9S-7 — Two builds in one process; C# against both builds (design, retrospective)

*Full-loop design, written after the light-lane implementation (`f1a099e`) was ejected at phase B. It records the
design as landed and lists every deviation from `plans/D9S-7-brief.md`. Cap 150 (ruling 2026-11-13). Ledger read in
full: 524 lines; highest entry line 516, "2026-11-14 — D9S-7: CI proves C# against the server build through a renamed copy".*

## Summary
Rung 9 proves d9 L5/L16/L17 (BC-15) with a real second shim. A process that links one build and loads the
other, in either order, gets one working shim. The later copy refuses with `FL_INTERNAL` and names both files,
yet still answers its variant, the ABI version and the marker. Rung 10 proves L13 from C#: the conformance lane
re-runs the C# rows against the server shim, which CI stages under the edge file name (ruling :516). Nothing
shipped changes.

## Design (as landed)
**Edge twin** (`c-abi/tests/CMakeLists.txt:273-315`; server variant, tests only). `fletcher-c-abi-edge-twin`
copies the edge target's recipe into the test CMake: the same four sources, `FL_VARIANT_EDGE`, hidden
visibility, and `exports.map` + `--exclude-libs,ALL` off MSVC. It links `fletcher-c-abi-impl` (so the marker
comes from the one shared source), pubsub and both providers, and no Arrow. It sits beside the server shim with
no `install()`, and `package()`'s patterns name only `fletcher-c-abi-server`, so it cannot ship. The edge
package's graph is untouched (BC-11; ruling 2026-11-13 "test-only Arrow refused too").

**Rung 9 executables** (`:317-344`). One source, `test_two_builds.cpp`, builds two executables that run from the
server shim's directory. `c_abi_two_builds_server_first` links the server shim and loads the twin by path;
`c_abi_two_builds_edge_first` does the reverse. Each gets the linked build as a literal (1 edge, 2 server) and
both file names from `$<TARGET_FILE_NAME>`. Test prefixes are `server-first.` and `edge-first.`.

**Rung 9 tests.** `Other()` loads the other file once per process (`LoadLibraryA` /
`dlopen(RTLD_NOW|RTLD_LOCAL)`), never unloads it, and resolves six entry points from that handle, never from
global scope. `RequireLoaded` fails the row if the load or any refusal-path name did not resolve.
- `TheLaterBuildRefusesNamingBothAndTheFirstKeepsWorking`:
  - the linked copy answers `kLinkedVariant`, and the loaded copy answers the other literal;
  - the loaded copy's `fl_provider_create` returns `FL_INTERNAL`, naming both files, and the copy that wrote
    the message frees it;
  - the linked copy's `fl_provider_create` returns `FL_OK`.
- `ARefusedBuildStillAnswersItsVariantAndVersion` asserts the refusal first. Then the refused copy's variant
  query answers its own build, its ABI version equals `FL_BINDING_ABI_VERSION`, and its marker text `STREQ`s
  the linked copy's (A32, which keeps L13's list true). The marker is resolved here, not in `RequireLoaded`,
  so a build that lacks it fails the refusal rows for what it did.
- Declared mutations, each meant to turn a row red: raise the scan threshold (L5); rename the marker export in
  one build (L17; in one load order the red is a link failure); run the check lazily (the first copy is
  poisoned too); route the variant query through the refusal (L16).

**Rung 10 lane** (`ci.integration-test.binding-abi-conformance.yml`, both legs):
- **Build.** The sparse checkout gains `pubsub-arrow` and `scripts`. The loop also creates `pubsub-arrow`, then
  a second `c-abi` with `-o '&:variant=server'`. `run_tests` defaults to False, so no tests run here
  (`ci.c-abi.yml` runs them).
- **locate_shim lookups (D9S-2 DEBT-1).** Each leg's shim lookup took the first matching file in the cache
  (`find … -print -quit`). The cache now holds both builds, so each is found by package identity
  (`locate_shim.sh fletcher-c-abi variant=edge`, `… fletcher-c-abi-server variant=server`), and the script
  refuses none or several. The emitter and `conanrun.sh` lookups stay `find`: they search the test's own
  build folder, not the cache.
- **Server pass**, after the edge pass: copy the server file into a fresh `mktemp -d` stage under the edge
  name; run `dotnet test -c Release -o <fresh dir> -p:FletcherNativeShim=<staged copy>` on the same corpus;
  then `cmp -s <out>/<edge name> <server file>` must report byte-identical, or the lane fails. No `dotnet/`
  or csproj edit.
- **cmp control, every run:** as landed in fix 1 (`157d3c3`, 4b S1); the workflow file is its one home.
- **Corpus.** One corpus, emitted by the edge build, is round-tripped by C# through each build, so the server
  pass compares server bytes with edge bytes. The emitter recipe is unchanged. This departs from ladder rung
  10's "passes the switch through" and is annotated there.
- **`ci.pr.yml`.** The lane's filter gains `pubsub-arrow/**` and `scripts/locate_shim.sh`.

## Corner cases forbidden
- **The server pass re-testing a stale edge file.** Refused: fresh output folder; `cmp` must pass; the control
  proves `cmp` can fail.
- **Picking the wrong build from a cache that holds both.** Refused: the lookup is by package identity, and
  none or several is an error.
- **Same build twice, a mis-staged file, or a failed load.** Refused: `RequireLoaded` or the two literal
  variant asserts fail the row. It never passes vacuously.
- **A "still answers" row about a healthy copy.** Refused: Test 2 asserts the refusal first.
- **A test call through the loaded copy binding to the linked one.** Unrepresentable: every entry point is
  resolved from the loaded copy's handle. The shim's own internal calls on Linux are under Limits.
- **Arrow reaching the edge build, or the twin shipping.** Unrepresentable: the twin is server-package,
  tests-only, links no Arrow, has no install, and no packaging pattern or CI glob matches its name.
- **Freeing a message with the wrong copy, or unloading the refused copy.** Unrepresentable: the message is
  freed by `other.error_dispose`, and the handle is a never-released function-local static.

## Premises and stop conditions
- **Both builds export the marker from `fletcher-c-abi-impl`, and L5's scan runs once, at each copy's load.**
  STOP-AND-ASK if either becomes per-variant or lazy: rung 9 would then be testing a different law.
- **The twin's recipe equals the shipped edge recipe.** Only the variant define is checked. A source, define or
  link added to the edge recipe must be mirrored in the same commit. STOP-AND-ASK if the twin ever needs Arrow.
- **C# loads only `fletcher-c-abi`** (`NativeLoader.cs:222-230`). When domain 10 settles Q5a, the staging rename
  is retired.
- **On Linux, the emitter's `conanrun.sh` covers the server shim's runtime closure.** The extras are
  pubsub-arrow (static) and Arrow (already in the emitter graph). If the staged shim fails to load, fix the
  environment; never drop the pass.

## Forcing-test mapping
| Test | Green by | Red for the right reason |
|---|---|---|
| `{server,edge}-first.TwoBuilds.TheLaterBuildRefuses…` | twin + 2 executables + existing L5 scan | threshold raised or marker renamed (no refusal); lazy check (first copy poisoned) |
| `{server,edge}-first.TwoBuilds.ARefusedBuildStillAnswers…` | same; existing L16 query | variant routed through the refusal; marker text diverging |
| Lane server pass (rung 10, L13) | stage + `dotnet test -o` + `cmp` | a C# row failing on the server shim; `cmp` red if the edge file reaches `<out>` |

Rung 9 is a guard rung, expected green on arrival (declared; ladder rung 9; audit 0b). **The mutations are
declared, but no log or commit records that they were executed.** The compliance review should run or waive them.

## Disclosed limits (Risks)
- **The twin is not the shipped edge file.** It is a same-source test rebuild in the server package. The marker
  export, which is what L17 needs, is identical. Recipe drift is caught only for the variant define. The L17 and
  BC-15 `[built]` marks carry this caveat (audit 0b N1).
- **The Linux `bash -c` path is unrun.** The server pass, the control and `locate_shim` ran only by hand on
  Windows (11/11). The Linux block was written blind inside an escaped `docker run` string, so its first CI run
  is its first execution. Rung 9 has not run on Linux either.
- **The emitter stays on edge.** C# round-trips through the server codec against edge bytes, but the C++
  emitter never runs on the server build.
- **Linux interposition (finding, not fixed).** In edge-first, the refused server copy's calls to its own
  exports can bind to the twin's. Only the refused copy is affected, and it refuses before any body runs
  (audit 0b N2).
- **No shipped coexistence window.** The staging rename is a CI-only bridge, deleted when domain 10 settles Q5a.

## Deviations from the brief
1. **Size, and the ejection.** CI YAML was declared at +30/−8 (about 38 touched lines). It landed at +77/−15
   raw, **65 non-comment touched lines**, over the light lane's 60, so the item was **ejected to the full loop**
   at phase B. The causes are comments, and a Linux `bash -c` block that repeats the Windows steps.
2. **Test volume.** Declared about +170. Landed: `test_two_builds.cpp` has 188 new lines and the test CMake about
   74 (`:272-345`), about +262 in all. These counts are read from the files, not git-counted; confirm with
   `git diff --numstat c3d9670..f1a099e`.
3. **How the control works:** superseded by fix 1 (`157d3c3`, 4b S1); see the workflow file.
4. **The path helper is copied, not shared.** The brief said "through the decoy's path helper"; `OtherPath()` is
   a 15-line copy of `test_single_copy.cpp:41-55` (`DecoyPath()`).
5. **Test 1 also pins the linked build's variant** to its literal. That is stronger than the brief, and it
   catches a mis-linked executable.

Nothing else deviates: the surface (0), the deleted lookups, where the twin sits, the corpus and the filter all
match the brief.

## Numbers
Declared: +30 / −8 CI YAML. **Actual: +77 / −15 raw, 65 non-comment touched lines, in 2 files.** Tests (not
shipped): about +262, in 2 files. New public surface: 0. Shipped code: 0.

## Files-to-touch / Files-to-delete
**Touch (as landed):** `c-abi/tests/test_two_builds.cpp` (new), `c-abi/tests/CMakeLists.txt`,
`.github/workflows/ci.integration-test.binding-abi-conformance.yml`, `.github/workflows/ci.pr.yml`; docs: d9
(L5, L17), the register (BC-15), the D9S ladder (rungs 9 and 10).
**Delete:** the lane's two shim lookups by "first matching file in the cache", replaced by
`scripts/locate_shim.sh` by package identity (D9S-2 DEBT-1). Nothing else: the decoy rows and the faked-refusal
containment rows are lawful and keep their own subjects (audit 0a); nothing is re-anchored.
