# D9S-3 — Stage Brief (2026-11-13)

**Lane:** full loop. The stage was ejected at phase A: about 75 touched lines against the 60-line admission. The design is in `plans/D9S-3-edge-arrow-free.md`.

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

**Handled:** two residues, each failing safe (design, Corner cases): the symbol control takes the first `c_abi_tests` found, since nothing is measured from it; the platform is chosen by file extension, and a wrong guess is refused, never passed.

**Red before the change:** none. The edge build is green by construction, because nothing links Arrow yet. Instead, two controls run every time and must go red, naming Arrow:
- the same edge build with tests switched on (its graph includes Arrow, which the law allows for tests);
- a test program that links Arrow.

## Decisions for you
None. The Windows scope limit is disclosed under Risks.

## Risks accepted / debt carried
- **Windows checks less than Linux.**
  - Windows has no `nm`, a Release DLL keeps no full symbol table, and no PDB is built. So the Windows symbol half sees only the Arrow class and import names inside the file.
  - An Arrow link that adds neither would pass that half on Windows. The graph half (every Conan path) and the size ceiling still catch it, and Linux always checks the full symbol table.
  - A PDB would close the gap, but it changes the edge DLL itself, which L12 forbids. A link map leaves the DLL as it is. But it is written to the build folder, not the package, so the check could not choose the edge build's map by its package the way it chooses the shim. And the Linux leg already reads the full symbol list. Not taken in this stage; the debt is recorded.
- **Evidence:** Windows is checked by hand at close. CI evidence waits for the owner's draft PR (ruling 2026-11-13). Until a green run exists, L8 and BC-11 are marked "Windows, by hand".
- **Premises (stop and ask if false):**
  - Test build folders stay in the Conan cache after a build; otherwise the symbol control has nothing to check.
  - The Linux edge shim ships unstripped, and the build image's `nm` demangles names. Neither has been tried for real yet: Linux was checked only on test files, in WSL.
  - On Windows this holds already: by hand, the edge shim carries 51 `@fletcher@@` names.
  - If any of these fails, stop. Do not drop or weaken the check.
- Docs, not counted: d9 L8 becomes `[built]` (no longer "no code owner"), Q4 is resolved, BC-11's graph and symbol check is marked built.

## Numbers
- Declared net lines (raw diff, comments included): about **+106 / −0**.
  - Script: +90 / −0, of which 59 are not comments.
  - `ci.c-abi.yml`: about +16.
- Touched production lines, not counting comments: about 75, in **2 files**. That is over the light lane's 60, so the stage runs the full loop. It is well under the 250 ceiling.
- New public surface: 0. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
