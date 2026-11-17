# D10C-11 — Stage Brief (2026-11-17)

**Lane: ejected from the light lane (152 touched lines vs 60); now on the full loop.** The design is in `plans/D10C-11-preview-from-packages.md`.
**In one sentence:** the round's purpose is proven from the shipped packages. An app that adds the server package gets batches from Fletcher's one batcher on the server shim. An app on the base package alone publishes, subscribes per row and decodes on the small shim, has no server shim in its output, and gets the named "not supported, add Eiva.Fletcher.Server" error when it asks for batches.
**Forcing test:** the preview-bundle consumer (T46, `native batch`), built from the packages in two configurations on Linux and Windows, plus the minimal-machine run on Linux. It closes the declared red window (ruling :650 (2)).

## Interfaces
| Surface | Change | Why |
|---|---|---|
| none public | — | Test harness only. The consumer program takes the configuration it was built as (`base` or `server`) as its second argument. |

## Deleted
Nothing. T46 is **re-anchored, not deleted** (audit 0a (1)): the old base-only "rows expected" arm is replaced by a refusal arm in the base configuration and a rows arm in the server configuration. The shim-file check becomes one check per configuration (0a (2)).

## Corner cases forbidden vs handled
**Forbidden:**
- A check that accepts either shim file (that would tolerate a forbidden fallback, d10 L5). Each configuration names its one expected file, and exactly one shim module may be loaded.
- A rows arm in the base configuration (d10 L6).
- A configuration argument that does not match the build: both directions fail loudly. A missing or unknown argument is refused at the door (usage, exit 2).
- On the server build below the floor, a refusal that names the edge file, which would mean it fell back.

**Handled:** none.

## Decisions for you
None. Whether to use one project with a switch or two copies is a mechanism choice. I took a copy plus `dotnet add package`, because it keeps the base project equal to the README and mirrors the user's own step.

## Risks accepted / debt carried
- **Floor premise, corrected:** the server shim's measured floor is `GLIBCXX_3.4.32`, the same as the edge shim's, but on Debian 12 the OS names `GLIBCXX_3.4.31`, the first unmet version it hits.
  - PM decision, recorded in the log: the server arm's below-floor check requires the floor sentence ("glibc 2.34 and the GCC 13 C++ runtime") and `GLIBCXX_3.4.3[12]' not found`.
  - The floor is not loosened, and the base arm keeps its exact 3.4.32 grep.
  - The floor stop-and-ask fires at `check_shim_dependencies.sh:59` on the server shim (DEBT-2).
- **Green on arrival:** everything except the old T46, because D10C-4, -5 and -7 built the behaviour. No production code changes in this stage.
- **Evidence:** the bundle was assembled locally, because CI has not run on this branch. A green CI run is owed before marks lift (:408 (2)), not before this close.
- **Docs** (audit 0a (4), done in phase 2): `README.package.md:145-154`, `PREVIEW-TESTERS.md:10`, the consumer README, and d10 L7's build mark.

## Numbers
Actual net lines: **+126 / −26** (152 touched) in `Program.cs`, `run.sh` and `minimal.sh`, against the ~50 I declared (≈ +40 / −12). Docs still to add: about +12 / −8 in 4 files. CI YAML: none. New public surface: 0. Design cycles used: 1/2.

---
**As landed (2026-11-17):**
Ejected (152 touched vs 60). Harness +139/−26 (Program.cs, run.sh, minimal.sh); docs about +50/−22; no production change. T46 re-anchored. Round purpose proven from a locally assembled bundle on Linux (container) and Windows (host); minimal.sh via a docker wrapper. PM: the server below-floor regex `GLIBCXX_3\.4\.3[12]' not found` (loader reporting order; the floor is proven by check_shim_dependencies.sh). Design cycles 1, fix cycles 1. CI unrun.
