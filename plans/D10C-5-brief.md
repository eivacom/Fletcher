# D10C-5 — Stage Brief (2026-11-16)

**In one sentence:** an app can add one package beside `Eiva.Fletcher` to get the server shim (the build with
the C++ batcher); an app that does not keeps the small, Arrow-free shim, and packing refuses any package that
breaks that split. Batches flow from stage 7; until then the server shim loads and works per row as today.
**Forcing test:** the package check over all five packed packages, plus seven breakages it must refuse — the
server package holds exactly the server shim (Linux, Windows) and needs the base package; the base package
still holds only the edge shim within 12 MB; nothing else holds native code. **Server package size limit:**
its first measured size (devcontainer pack, CI's image) + 4.3 MB (the margin the base package's 12 MB left), rounded up to a whole MB.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| NuGet package `Eiva.Fletcher.Server` (name: Decision 1) | NEW | what an app adds to get batches (ruling 2026-11-13, "Separate server package") |
| Release assets (two server lockfiles); package README | NEW / CHANGED | a user can rebuild the server shim they were shipped (LGPL route); README lists five packages |

## Deleted
Only wording ("only `Eiva.Fletcher.Interop` may carry native code"), replaced by the five-package rule.
## Corner cases forbidden vs handled
**Forbidden** (packing refuses): a server package missing a platform's shim or holding anything else; either
shim in the other's package; a server package not needing the base one; **any package bringing the server shim
to an app that did not ask for it**; the wrong licence notice in either shim package; Linux and Windows server
shims from different libraries; a server shim needing more than the stated system libraries and Linux; a server
package past its limit. Server and base packages speaking different native interface versions are refused at
load, as today; same-interface version skew loads (compatible by design). **Handled:** none.

## Decisions for you
1. **What do users type to get batches?**
   (a) **`Eiva.Fletcher.Server`.** Users: add it beside `Eiva.Fletcher`; matches "server build" in the loader's
   messages and the shim's file, though a newcomer may first read it as the gateway. Architecture: names the
   build, not a feature, so later server-only features land in it (d10 L6, L15).
   (b) **`Eiva.Fletcher.Interop.Server`.** Users: reads as plumbing, and the README says not to reference Interop
   directly, yet this one they must. Architecture: mirrors the base package; strains the round purpose.
   (c) **`Eiva.Fletcher.Batching`.** Users: says what they get, but it also serves per-row work. Architecture:
   names a feature, not a build; strains d10 L6 and diverges from "server" everywhere else.
   **Recommendation:** (a), from the round purpose (one obvious package for batches) and d10's term. **Default:**
   (a). *Background: d10 leaves the id to this stage; a package is new public surface.*
2. **May the older packaging decisions saying "four packages" / "one shim" be annotated?**
   (a) **Annotate D-BIND-134, -139, -140, -142, -173 "extended to five packages / two shims", text kept.** Users: none.
   Architecture: in step with ruling 2026-11-13 (:337 (1)), trail intact. (b) **Leave them; a close finding.** Users:
   none; maintainers read "four" meanwhile. Architecture: tier 2 out of step with the ruling for the round.
   **Recommendation:** (a), the ruling governs and an annotation loses nothing. **Default:** (a).

## Risks accepted / debt carried
- **Stops and asks you** if the server shim needs a library or Linux floor beyond the stated ones, or Arrow C++'s
  notice is missing. Licence-exception wiring must skip the server package. A new id's first nuget.org push is unproven.
- Verification: devcontainer (`fletcher-build:latest`, volumes `fletcher-d10c4-conan`/`-ws`) and Windows by hand:
  `check_shim_dependencies.sh` on both shims; pack five packages; `check_dotnet_packages.sh` + the seven breakages
  (`scripts/test_check_dotnet_packages.py`); then CI on your draft PR, which re-confirms the size limit.
## Numbers
Declared net lines: +210 / −55 (net ≈ +155; no split) · new public surface: 1 (the package) · design cycles: 2/2

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
