# D9S-3 — Stage Brief (2026-11-13)

**Light lane: fits.** About 45 production lines are touched, not counting comments, in 2 files. The limit is 60 lines and 3 files.

**In one sentence:** CI fails if Arrow C++ can reach the edge build, and the failure names the culprit. Two paths are checked: the edge build's dependency graph, and the names inside the edge shim. Until now only a size ceiling stood in the way. The server build is not judged.
**Forcing test:** the new CI step "the edge build carries no Arrow C++ (d9 L8)" on both legs, also runnable by hand on Windows. It proves that no Arrow package and no Arrow name reaches the edge build. It must keep passing when D9S-4 links Arrow into the server build (ladder guard c).

## Interfaces
| Surface | Change | Why |
|---|---|---|
| CI c-abi lane, Windows and Linux legs | NEW step | the edge build is checked for Arrow by name (L8, BC-11, Q4) |
| Shim, recipe, headers, edge or server artefacts | unchanged | L12 rules out any other change to the edge build |

## Deleted
Nothing. The size ceilings stay as the coarse check: L12 says no size check changes. This stage only adds a check beside them, as rung 3 plans.

## Corner cases forbidden vs handled
**Forbidden** (the check refuses, and says why):
- A listing that read nothing passes. Refused if the shim yields no Fletcher name (e.g. a stripped `.so`) or the graph has no `fletcher-c-abi` root.
- A false hit on nanoarrow, the edge codec. Patterns match only the `arrow` namespace: `nanoarrow::` and `@nanoarrow@@` cannot match.
- The wrong file is checked. The edge shim is picked by its package identity (`locate_shim.sh`), never by file name.
- The check is trusted without proof. Every run includes both controls, and each must go red and name Arrow; otherwise the step fails.
- Arrow linked as a shared library. Linux lists undefined `arrow::` references too; on Windows, imported Arrow names carry `@arrow@@`.

**Handled:** none.

## How   (light lane: this section is the design)
- **New `scripts/check_edge_arrow_free.sh <recipe dir> <profile> <edge shim>`** (bash).
- **Graph half:** run `conan graph info` on the recipe with `variant=edge` and the shipped defaults (no tests, no probe), output as JSON. Parse it with `python` (or `python3`). Fail if any host node is `arrow`, `fletcher-arrow-bridge` or `fletcher-pubsub-arrow`, and list each one by name.
- **Symbol half, Linux:** `nm -C` on the unstripped edge `.so`, all symbols. Fail on `(^|[^A-Za-z0-9_])arrow::`; the listing must contain `fletcher::`.
- **Symbol half, Windows** (`.dll`/`.exe`): `grep -aoE '[A-Za-z0-9_?$@]*@(arrow|fletcher)@@'` on the image. This finds the type names and decorated import names the image carries. Fail on `@arrow@@`; the listing must contain `@fletcher@@`.
- **Graph control:** the same graph with `run_tests=True` added. It must name `fletcher-arrow-bridge`.
- **Symbol control:** a `c_abi_tests[.exe]` binary found under the cache's `p/b`. It links Arrow (T10), so it must yield an `arrow` name. If none is found, the step fails. Either build's binary will do, so which one it picks does not matter.
- **`.github/workflows/ci.c-abi.yml`:** Windows gets a new step after the export comparison. Linux gets one call inside its existing docker block, after the export control.
- **Red before the change:** none. The edge build is green by construction, because nothing links Arrow yet. The controls are the red, on every run.

## Decisions for you
None. The Windows scope limit is disclosed under Risks.

## Risks accepted / debt carried
- **Windows checks less than Linux.**
  - Windows has no `nm`, a Release DLL keeps no full symbol table, and no PDB is built. So the Windows symbol half sees only the Arrow class and import names inside the file.
  - An Arrow link that adds neither would pass that half on Windows. The graph half (every Conan path) and the size ceiling still catch it, and Linux always checks the full symbol table.
  - A link map or PDB would close the gap, but it would change how the edge build links, which L12 forbids. Rejected.
- **Evidence:** Windows is checked by hand at close. CI evidence waits for the owner's draft PR (ruling 2026-11-13). Until a green run exists, L8 and BC-11 are marked "Windows, by hand".
- **Premises (stop and ask if false):** (1) test build folders stay in the Conan cache after `conan create`; otherwise the symbol control has no binary. (2) The Windows edge shim carries at least one `@fletcher@@` name; this is checked by hand before the stage commits. In either case, stop rather than drop or weaken the check.
- Docs, not counted: d9 L8 becomes `[built]` (no longer "no code owner"), Q4 is resolved, BC-11's graph and symbol check is marked built.

## Numbers
- Declared net lines (raw diff, comments included): about **+68 / −0**.
  - Script: about +52.
  - `ci.c-abi.yml`: about +16.
- Touched production lines, not counting comments: about 45, in **2 files**.
- New public surface: 0. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
