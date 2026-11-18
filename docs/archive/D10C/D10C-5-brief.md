# D10C-5 — Stage Brief (2026-11-16)
**In one sentence:** an app can add one package beside `Eiva.Fletcher` to get the server shim (the build with the C++
batcher); an app that does not keeps the small, Arrow-free shim; packing refuses any package breaking that split (batches flow from stage 7).
**Forcing test:** the package check over all five packed packages, plus fourteen breakages it must refuse (seven designed; C8–C14 added by 0b and review) — the
server package holds exactly the server shim (Linux, Windows) and needs the base package; the base package
still holds only the edge shim within 12 MB; nothing else holds native code. **Server package size limit:** its
first measured size (devcontainer pack, CI's image) + 4.3 MB (the margin the base package's 12 MB left), rounded up to a whole MB.
## Interfaces
| Surface | Change | Why |
|---|---|---|
| NuGet package `Eiva.Fletcher.Server` (name: Decision 1) | NEW | what an app adds to get batches (ruling 2026-11-13, "Separate server package") |
| Release assets (two server lockfiles); package README | NEW / CHANGED | a user can rebuild the server shim they were shipped (LGPL route); README lists five packages |
## Deleted: only wording ("only `Eiva.Fletcher.Interop` may carry native code"), replaced by the five-package rule
## Corner cases forbidden vs handled
**Forbidden** (packing refuses): a server package missing a shim or holding anything else; either shim in the other's
package; a server package not needing the base one; **any package bringing the server shim to an app that did not ask
for it**; a wrong licence notice (platforms: Decision 3); a server shim needing more than the stated system libraries and
Linux; a server package past its limit. Different native interface versions are refused at load; same-interface skew loads. **Handled:** none.
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
3. **Linux's server shim is built from one library Windows's is not (libbacktrace, BSD-3-Clause, via Boost). How
   does the package's licence notice cover both platforms?** (a) **One notice per platform.** Users: two notices in
   the server package, each exact for its platform. Architecture: most exact for D-BIND-134 ("lists the libraries
   linked into the shim"); strains D-BIND-140/-142's one notice at the root; the server package departs from the base's shape.
   (b) **One notice listing both platforms' libraries.** Users: one notice, as in every package; Windows users also read
   libbacktrace's licence for code their shim lacks. Architecture: serves D-BIND-140 (one graph-derived notice per
   package) and the notice tool's rule that it lists the dependency graph, not only what is linked; omits no text
   D-BIND-152/-166 require. (c) **Build the server shim without Boost's backtrace.** Users: identical notices; no visible
   change unless the shim uses Boost's backtrace stack traces (not verified). Architecture: a domain-9 recipe or shared-profile
   edit this round forbids, so your named scope exception (as :435); a CI-only switch makes the shipped shim differ
   from what the relinking route rebuilds (strains D-BIND-140, -173).
   **Recommendation:** (b): D-BIND-140's one notice stays, no licence text is lost, domain 9 untouched. **Default:** (b).
   *Background: Linux-only `boost/1.90.0` → `libbacktrace/cci.20210118`; under (b) the pack job's identical-list check
   (`ci.dotnet.yml:497-502`) becomes "each platform's list ⊆ the shipped list"; compliance F3.*
## Risks accepted / debt carried
- **Stops and asks you** on a library or Linux floor beyond the stated ones, or a missing Arrow notice. Licence-exception wiring skips the server package. Verification: devcontainer + Windows by hand, five packs, the check + seven breakages; CI re-confirms the limit.
## Numbers — declared net lines: +210 / −55 (net ≈ +155; no split) · new public surface: 1 (the package) · design cycles: 2/2

---
**As landed (2026-11-16):**
Counted prod +337/−77, net +260 (declared ~+155; ceiling 250; owner accepted the overrun, ledger 2026-11-16). Tests: 14 controls (+339). Server pack 12,848,432 B, so `SERVER_CEILING` = 18,000,000. Not predicted: P4 breached (Linux-only libbacktrace), resolved by the union-notice ruling and `check_notice_covers.sh`; README rebuild route fixed and rehearsed from an empty Conan home. Design cycles 2, fix cycles 1. CI unrun.
