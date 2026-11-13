# D9S-2 — Stage Brief (2026-11-13)

**In one sentence:** the shim now comes in a second build, made from the same sources. When asked, it says "server". A program written for today's shim behaves the same on it, and both builds offer exactly the entry points the header declares. CI measures the embedded build's size and dependencies on that build alone, chosen by which package it is, not by guessing from a file name. The embedded build is unchanged, and the server build carries no Arrow C++ yet, because batching only arrives in D9S-4.
**Forcing test:** `test_package` run against each package. It shows that the package recorded as the server build answers "server" and the edge package answers "edge". Alongside it, the whole existing shim test suite runs a second time against the server build, and CI checks on every run that both builds offer the same entry points as the header.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Build choice when consuming the shim package: `edge` (default) or `server`, part of the package's identity | NEW | "One shim in two variants" (2026-11-12). Apps that ask for nothing still get today's build |
| The server build's file: its own name (`…-server`), beside the edge file's unchanged name | NEW | Lets both builds sit side by side (Decision 1) |
| "Which build are you?" answers "server" on the server build | CHANGED (new answer on a new build) | d9 L16. The edge build and the test-only probe build still answer "edge" |
| CI: the edge shim is chosen by its package; both builds' entry points are compared with the header | CHANGED | d9 L7, L12. A server package in the same cache can never be measured as the edge one |

## Deleted
- The CI steps that found the shim by file name (four places), replaced by selection by package.
- The Linux-only check of exported names, replaced by one check that covers both builds and both platforms against the header. It still refuses any stray export.
- A stale CI note saying the shim "exports one function".
- No tests are deleted. The two "which build" tests now expect each build's own answer, never "either".

## Corner cases forbidden vs handled
**Forbidden:**
- A shim that does not state its build will not compile.
- A test that has no expected build, or accepts either build, will not compile or run.
- A third build name is refused.
- A package holding two builds (server plus the test probe) is refused.
- CI refuses to measure when more than one shim could match.
- An export check that read nothing fails.
- If the default ever stopped being the edge build, CI goes red.
- Arrow C++ cannot reach either build in this stage, because nothing asks for it.

**Handled:** on Windows, only the `fl_` entry points are compared. The static Fast DDS library adds thousands of exports that cannot be suppressed (accepted 2026-09, D-BIND-157).

## Decisions for you
1. **When an app or device image carries both builds, should the server build be a differently named file?**
   (a) **Yes, its own name.** *Users:* both files can sit in one folder, an operator can tell from the file which build is deployed, and a process that loads both refuses the second one by name. *Architecture:* this serves one-shim-per-process (d9 L17), the two-package ruling (D22), and "a C# app loads the server build only if it references the server package" (D29), because the app picks a build by its file. It leaves the .NET package names to the C# switch round, which must teach C# the new name or rename the file when packing.
   (b) **Same name as today.** *Users:* C# finds either build with no change. But unless the C# switch round renames one file when packing, the two cannot share a folder: whichever is copied last silently wins, so an app that references both packages may run the edge build and get "not supported" on batches. *Architecture:* this strains L17 and D29, because which build loads would depend on copy order, not on the package referenced. It also blocks the test that loads both in one process (stage 7).
   **Recommendation:** (a). The purpose wants the embedded build kept exactly as it is and the server build unmistakable, and L17 needs both to be loadable to prove that only one runs.
   **Default if you don't answer:** (a).
   *Background (skippable): the d9 open question Q5a gives the native file name to this round and the package names to domain 10.*

## Risks accepted / debt carried
- Stage 7 has two consequences of (a). Its "both in one process" test must bring an edge copy into the server build's test tree, or receive the other package's path. Its C# conformance run must stage the server file under the name C# looks for (in CI only, no C# edit).
- The c-abi CI lane builds the shim and its tests twice per platform, so it takes longer.
- D9S-1's leftover "the version and build answer in a refused shim" moves to stage 7. The only real refused shim is the second shim loaded into a process, which is that stage's test. A test here would only poison the test program's own copy and prove nothing.
- The edge package's identity hash changes because the recipe gains an option. The edge binary does not change.

## Numbers
Declared net lines: **+266 / −101**, counted as the raw diff will count them. Production (recipe, CMake, C source, CI workflows, scripts, comments included) is **+197/−68, net +129, in 8 files**. Tests are +33/−12 and docs about +36/−21. New public surface: 2. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
