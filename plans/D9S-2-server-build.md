# D9S-2 — The shim's server build (design)

Round D9S, domain 9 ([d9](../docs/domains/d9-c-abi-shim.md)), ladder rung 2 ([D9S-test-ladder.md](D9S-test-ladder.md)).
Tree `cf22af8`. Rulings ledger read top to bottom: 393 lines, last heading "2026-11-13 — D9S pre-round:
a failed handoff is reported; corrections approved". Step-0a: [D9S-2-audit-0a.md](reviews/D9S-2-audit-0a.md) (CLEAR).

## Summary

One recipe, one source tree, two Conan packages: a package option `variant` (`edge` default, `server`)
that stays in the package ID. It decides the one shim the package holds, the file's name
(`fletcher-c-abi` / `fletcher-c-abi-server`) and the answer of `fl_binding_variant` (BC-10's
per-target constant). The edge-surface suite, the per-artefact variant rows and `test_package` run
against both packages. CI picks every shim by the package it is in, never by file name. It compares
each build's export table with `binding.h`'s declarations. **The server build links no Arrow C++
and requires no `pubsub-arrow` yet** (see "Arrow at rung 2").

## Design

### The switch (BC-11's site, BC-10's input)

- `c-abi/conanfile.py`: `options["variant"] = ["edge", "server"]`, default `"edge"`. It is **not**
  deleted in `package_id` (only `run_tests` is), so an edge and a server package can never share an
  identity (audit F13). `generate()` passes it as the cache variable `FLETCHER_SHIM_VARIANT`.
  `package()` copies only the variant's own file: the three existing patterns are rebuilt from one
  stem (`fletcher-c-abi` or `fletcher-c-abi-server`), so the edge patterns stay byte-identical to
  today's. `package_info().libs = [stem]`. The CMake target name stays `fletcher-c-abi::fletcher-c-abi`
  for both, so a consumer chooses the build with the option and changes nothing else (this is what
  rung 10 needs).
- `validate()` refuses `variant=server` together with `with_probe_shim=True`. The probe is an edge
  artefact (L13: three built-ins plus the probe, and it answers edge). A server package that carried
  it would hold two builds under one identity, and BC-11's "one package, one answer" would break.

### The artefact (one target, two names)

- `c-abi/CMakeLists.txt`: `FLETCHER_SHIM_VARIANT` is a cache STRING, default `edge`. Any value other
  than `edge` or `server` is a `FATAL_ERROR` (the door for a direct CMake configure; Conan already
  refuses an unknown option value). The shipped target `fletcher-c-abi` gets
  `FLETCHER_C_ABI_VARIANT=FL_VARIANT_EDGE` or `=FL_VARIANT_SERVER`. Under server it also gets
  `OUTPUT_NAME fletcher-c-abi-server`, which renames the DLL, its import library and the `.so` (and
  so its SONAME). The probe target always gets `FL_VARIANT_EDGE`. **Nothing else in the edge path
  changes**: same sources, links, visibility, version script and file name (L12).
- `c-abi/src/binding_version.c`, BC-10's single site:
  `fl_variant fl_binding_variant(void) { return FLETCHER_C_ABI_VARIANT; }`, preceded by
  `#ifndef FLETCHER_C_ABI_VARIANT` / `#error`. An artefact that does not state its build does not
  compile. Its comment states that the site is now the per-target definition and that D9S-4's
  refusals call this function. The site moves exactly where the register's BC-10 row already says it
  lives, so the register only gains a build mark.

### Q5a, shim half: the server file has its own name

`fletcher-c-abi-server` keeps the substring `fletcher-c-abi` that `SingleCopy.*` relies on (audit,
"Checked and LAWFUL"). Both files can sit in one folder and be loaded by one process (rung 9), and
no existing pattern or `find` that names the edge file can match the server file. It is put to the
owner as Decision 1 because the choice is visible to whoever deploys both builds. The NuGet-level
name stays domain 10's: a `.csproj` can pack the file under any name (Q5a, D-D9S-8).

### Arrow at rung 2: none (decided, as the brief asked)

The server package requires nothing the edge one does not. Reasons:
1. **L8 and the ladder's accumulation.** Rung 3's structural Arrow-free check must be in place
   before any Arrow requirement exists in the recipe ("The Arrow-free check (rung 3) is in place
   before the first Arrow link (rung 4)"). An option-gated `requires` is the exact route by which a
   mis-gated condition would put Arrow into the edge graph. It should first appear in the stage that
   uses it, behind a check that exists.
2. **L12/L13: the server build only adds, and at rung 2 it has nothing to add.** A dependency with no
   user changes the server graph twice, now and at D9S-4, and proves nothing in between.
3. Ladder rung 2: "No Arrow link yet."
So rung 2's server artefact is the edge sources with a different answer to one question. D9S-4 adds
`pubsub-arrow` under `variant == "server"`, together with its first user.

### Choosing a shim in CI by the package's identity (T3, T4, T5, T19; audit F8)

- New `scripts/locate_shim.sh <file stem> <option=value>...` prints the path of the one file named
  `<stem>` (`bin/<stem>.dll` or `lib/lib<stem>.so`) inside the one package whose `conaninfo.txt`
  records **every** given option (read with `\r` stripped). It fails, naming what it found, on zero
  matches or on more than one. It never picks "the first". The cache root is
  `${CONAN_HOME:-$HOME/.conan2}`, as the lanes already assume.
- `ci.c-abi.yml`, both legs:
  - **The server package is created first**, then the edge one: `conan create … -o
    "&:run_tests=True" -o "&:variant=server"`, then today's create, unchanged. The edge create passes
    no `variant`, so it relies on the default, and this is a standing **guard that the default is
    edge**: any other default leaves no edge package, and the next line is red. The audit's X-1 and
    X-2 lanes depend on that default.
  - `SHIM=$(locate_shim.sh fletcher-c-abi variant=edge with_probe_shim=True)` replaces the
    `find … -print -quit` (win `:91-96`, linux `:157-162`). The baselines and ceilings are unchanged
    (L12: no size check changes). The error text now says "the **edge** shim", because D3 is
    superseded for the server build. The glibc floor is still printed for `$SHIM`.
  - The export comparison (below) replaces the inline T4 block (`:231-247`) and also runs on Windows,
    where it needs `dumpbin` (found with `vswhere`, as `ci.dotnet.yml:306-312` does).
  - `scripts` is added to both legs' sparse checkouts, and the stale note at `:23-27` ("an ABI that
    exports one function") is corrected.
- `ci.dotnet.yml`, both legs: `SHIM` and `PROBE` are located with
  `locate_shim.sh fletcher-c-abi[-probe] variant=edge with_probe_shim=True`. This replaces the four
  `find` blocks (`:137-148`, `:285-298`). The strip check (T19), the dependency check (T5), the notice
  and the lock (X-2, default options) run unchanged on that file.
- `ci.pr.yml`: both new scripts go in the `c-abi` filter and `locate_shim.sh` goes in the `dotnet`
  filter, in the same commit (that file's rule).

### The export table equals the header on both builds (L7, L12, BC-10; audit F7; D9S-1 n3)

New `scripts/check_shim_exports.sh <binding.h> <edge shim> <server shim> [dumpbin]`:
- **Declared set:** every name declared `FL_ABI_EXPORT` in `binding.h`, taken as the identifier just
  before the first `(` on each `^FL_ABI_EXPORT` line (every declaration has its name on that line).
  If the parse does not contain both `fl_binding_abi_version` and `fl_binding_variant`, the check
  fails: the header was not read. This line names the variant query (n3).
- **Per shim:** on Linux, `nm -D --defined-only`. Anything outside `fl_*` and the linker's own five
  names is refused, as T4 refused it before (this keeps its subject). On Windows, the `fl_` subset of
  `dumpbin /exports`, because the Fast DDS archive's 3531 exports are accepted (D-BIND-157).
- **Equality:** the edge set equals the declared set, and the server set equals the declared set,
  each with the `diff` printed on failure. Edge = server follows. Any entry point exported by one
  build only is red by name. That is the guard for D9S-4 onward, where the ladder needs both builds
  to export the same set at every commit.
- **Standing control in the c-abi lane:** the same script is run with the probe shim in the server's
  place. It must fail, and its output must name `fl_test_probe_loan`. The probe's two extra `fl_test_*`
  exports are a real "export on one artefact only", so the comparison is shown to catch one on every
  run, not only once.

### Tests (not production)

- `tests/CMakeLists.txt` gives `c_abi_tests` its own mapping:
  `FLETCHER_EXPECTED_VARIANT=1` for edge and `=2` for server, written as literals, separate from the
  shim's own definition. `c_abi_probe_tests` gets `=1`.
- `test_binding_abi.cpp`: the two D9S-1 rows are **re-anchored, not loosened** (audit BA-V). They
  compare with `FLETCHER_EXPECTED_VARIANT`, which is a single per-artefact value, never "edge or
  server". If it is undefined, the file `#error`s. The wire values `FL_VARIANT_EDGE == 1` and
  `FL_VARIANT_SERVER == 2` stay asserted as literals.
- `test_package`: `test()` passes the package's recorded option
  (`self.dependencies["fletcher-c-abi"].options.variant`) to `example` as `argv[1]`. `example.c`
  fails unless the shim's answer names exactly that build, and fails when no expectation is given.
  This is the external check that ties the package's identity to the shim's own answer, and no CMake
  mapping is involved.
- In the server create, `c_abi_tests` is **the edge-surface suite built a second time against the
  server artefact**: every row, including `TheShippedShimHasNoProbe` (T11), `NanoarrowCodec.*` (T13),
  `Subscriber.*`, `Attachments.*`, `TopicOptions.*` and `Publisher.*`.

### D9S-1 n1 (a variant row in a refused shim): moved to D9S-7, not added here

`test_containment.cpp`'s `PoisonedForThisTest` poisons the **test executable's own** copy of the
internals (it links `fletcher-c-abi-impl`), not the loaded shim. A row built on that fixture would
pass whether or not the shim answers when refused, so it would prove nothing. The only faithful way
to get a refused shim is to load a second shim into the process, which is rung 9's subject ("In the
refused one, the version and the variant query still answer"). It goes to D9S-7, with this reason
recorded.

## Corner cases forbidden

1. **An artefact that does not say which build it is.** It cannot be built: `#error` in
   `binding_version.c` (rung 1).
2. **A test executable with no expected build, or one that accepts either build.** It cannot be
   built: `#error` in `test_binding_abi.cpp`. `example` with no expectation fails (rungs 1 and 2).
3. **A third build value.** Refused at the door: Conan's option domain, and CMake's `FATAL_ERROR`
   (rung 2).
4. **A package that holds two builds** (server plus probe). Refused by `validate()` (rung 2).
5. **A server package passing for the edge one, or the reverse.** Cannot happen: `variant` is in
   the package ID, the file names differ, and CI selects by the recorded option (rung 1).
6. **CI measuring an arbitrary one of several matching shims.** Refused: `locate_shim.sh` fails on
   more than one match (rung 2).
7. **An export comparison that read nothing.** Refused: the declared set must contain both version
   entry points, and a shim's export list must parse non-empty (rung 2).
8. **Arrow C++ in the server, and so near the edge, before rung 3.** Cannot happen at this stage:
   the recipe has no requirement for it (rung 1).
9. **A default other than edge** (X-1 and X-2 would silently describe or measure the server). Red:
   the c-abi lane's edge create passes no variant, so its edge locate fails (guard).

**Handled residue:** on Windows the export comparison checks only the `fl_` subset. *Why not
forbidden?* The static Fast DDS archive carries `/EXPORT` directives that no `.def` file or flag
suppresses (measured at BIND-0; accepted, D-BIND-157).

## Premises and stop conditions

- **P1.** Conan 2 writes the recipe's own options as `name=value` lines under `[options]` in each
  package folder's `conaninfo.txt`. *If false:* read the created package's `package_folder` from
  `conan create --format=json` instead. That is the same identity read another way, and the
  implementer may switch without a design change. **STOP-AND-ASK** if neither gives the package
  folder.
- **P2.** `OUTPUT_NAME` renames the DLL, its import library and the `.so` together, and CMakeDeps
  consumers of `fletcher-c-abi::fletcher-c-abi` link `libs = ["fletcher-c-abi-server"]`. *If false:*
  stop and report. Giving the server target its own name would touch `tests/CMakeLists.txt` and
  every consumer. That is a design change, not a workaround.
- **P3.** No consumer outside the c-abi lane resolves `fletcher-c-abi` with a non-default `variant`.
  Verified by grep at `cf22af8` (audit X-1 and X-2). *If one appears:* that lane's file-name `find`
  must be re-anchored with `locate_shim.sh` in the same commit.
- **P4.** The edge-surface suite has no row whose expected answer depends on the build except the
  two variant rows. Verified: the audit lists only BA-V. *If the server run shows another:* stop and
  report. It would be an L13 finding, not a test to loosen.
- **P5.** Two creates of one recipe in one job, with distinct package IDs, do not disturb each
  other's packages. This is Conan's model. *If they do:* stop and ask before splitting the lane.

## Forcing-test mapping

| Forcing test | Turned green by | Red for the right reason before |
|---|---|---|
| `test_package` (`example`) against each package, told the build its package records | the option, the per-target definition, and `binding_version.c` reading it | `conan create -o "&:variant=server"` is refused, because the option does not exist: there is no server artefact to ask |
| `BindingAbi.TheVariantIsAnsweredBeforeAnyOtherCall`, `…AnswerAlikeFromManyThreads` (edge, probe, server; per-artefact expectation) | as above, plus the test-side mapping | no server configuration to build them in. A wrong mapping (server definition on the edge target) turns the edge rows red |
| All of `c_abi_tests` in the server create (L13) | the second package built from the same sources | no server artefact. After the change it is green on arrival by construction, and declared so: it is the guard that D9S-4+ must keep green |
| `check_shim_exports.sh` edge = server = header (both legs) | the script and the server package | no server shim to compare. **Controls:** the probe in the server's place is red by name on every run. Once, in evidence: a throwaway `fl_` function compiled into the server target only is red by name |
| Edge shim chosen by identity (c-abi and dotnet lanes) | `locate_shim.sh` | file-name `find -print -quit`. **Control:** the server package is created first on every c-abi run. Once, in evidence: two edge packages (with and without probe) in one cache. `variant=edge` alone is refused as ambiguous, and with `with_probe_shim=True` it picks the probed one |

## Risks / Unknowns

- **R1 — D9S-7 (plan-shape).** Rung 9 needs an edge and a server shim in one process. They are now
  in two packages, so one test tree no longer holds both. D9S-7 can add a test-only edge artefact to
  the server configuration's test tree, from the same sources (the decoy precedent), or CI can pass
  the other package's path. That is about 10-20 lines, within D9S-7's estimate. Rung 10 loads the
  server shim into the C# lane: `FletcherNativeShim` stages the file under its own name, and the C#
  loader asks for `fletcher-c-abi`, so D9S-7 stages the file under the loader's name (CI only, no
  `dotnet/` edit). Both follow from Decision 1(a).
- **R2 — CI time.** The c-abi lane compiles the shim and its suite twice per platform. The
  dependencies are shared within the job. This is accepted as the cost of rung 2's own subject.
- **R3 — The C# loader finds only `fletcher-c-abi`.** Domain 10 (the C# switch round) either loads
  the server name or renames the file when packing. That is BC-10's deferred half, and nothing
  changes under `dotnet/`.
- **R4 — n1 moved to D9S-7** (reason above). **n3 closed here.** `ci.c-abi.yml:25` is closed here.
- **R5 — The edge package's ID changes** because the recipe gains an option, while the binary stays
  the same. No lane pins a package ID (checked: the lanes resolve by options).
- No coexistence window. The X-1 lanes keep file-name lookups on purpose: they are lawful while the
  default is edge and the names differ. The guard in corner case 9 and P3 hold that condition.
- No STOP-AND-ASK raised. Arrow does not reach the edge build, no edge entry point changes its
  answer, nothing under `dotnet/` changes, no domain 7 or 2 code changes, and no status number is
  added.

## Files-to-touch

Production (counted, comments included):
- `c-abi/conanfile.py`: option, default, `validate`, `generate`, stem-based `package`,
  `package_info` (~+24/−5)
- `c-abi/CMakeLists.txt`: cache variable and check, per-target definitions, `OUTPUT_NAME` (~+24/−1)
- `c-abi/src/binding_version.c`: `#error` guard, constant, comment (~+8/−4)
- `.github/workflows/ci.c-abi.yml`: server create, locate, scoped message, export step on both
  legs, probe control, sparse checkout, stale note (~+48/−36)
- `.github/workflows/ci.dotnet.yml`: four locates (~+10/−22)
- `.github/workflows/ci.pr.yml`: filters (+3)
- `scripts/locate_shim.sh` (new, ~+30), `scripts/check_shim_exports.sh` (new, ~+50)

Tests: `c-abi/tests/CMakeLists.txt` (~+10), `c-abi/tests/test_binding_abi.cpp` (~+12/−8),
`c-abi/test_package/conanfile.py` (~+3/−1), `c-abi/test_package/src/example.c` (~+8/−3).

Docs (not counted): d9 L7, L12, L13 and L16 build marks; §2 "Variant" and "Server variant" rows;
§4 "Builds"; Q4 and Q5a notes (CI by package identity built; the server file's name); `c-abi/README.md`
"Building"; register BC-10 (per-target constant and export check `[built]`) and BC-11 (selection by
package identity `[built]`; graph and symbol check D9S-3).

**Declared:** production **+197/−68** (net +129, 8 files); tests +33/−12; docs about +36/−21;
total about **+266/−101**. New public surface: **2**, the `variant` package option and the
server-build file name. No header change.

## Files-to-delete

- `ci.c-abi.yml:91-96` and `:157-162`, `ci.dotnet.yml:137-148` and `:285-298`, the file-name
  `find … -print -quit` lookups. Replaced by `scripts/locate_shim.sh` (package identity).
- `ci.c-abi.yml:231-247`, the inline export grep, `/workspace/shim-exports.txt`, and the lone
  `grep -q "^fl_binding_abi_version$"`. **Re-anchored, not dropped:** their subjects (nothing outside
  `fl_` is exported, and the version query is exported) are asserted by `check_shim_exports.sh` for
  both builds, and set equality to the header still fails without `fl_binding_abi_version`.
- `ci.c-abi.yml:23-27`, the stale note "exports one function". Replaced by a correct sentence.
- `binding_version.c:16`, the literal `FL_VARIANT_EDGE`. Replaced by the per-target constant.
- Tests: **none deleted.** The two D9S-1 variant rows are re-anchored to a per-artefact expectation
  (audit BA-V), and `example.c`'s "either build" check is tightened to the package's own build.
