# D10C-5 — The server package (ladder rung 5)

Round D10C, domain 10, branch `feature/csharp-switch`, HEAD `7e5a1c4`. Ladder: `plans/D10C-test-ladder.md`
row 5 + "Ground rules". Step-0a audit: `plans/reviews/D10C-5-audit-0a.md` (CLEAR; its five notes bind
this design). Citations: `d10`, `d9` (domain docs), `rul:N` (`plans/RIX-rulings.md`, read in full: **603
lines, 45 entries, last `## 2026-11-15 — D10C ladder: the domain-7 port tests` at :595**), `reg` (BC-17),
`CDP` = `scripts/check_dotnet_packages.sh`, `CI` = `.github/workflows/ci.dotnet.yml`.

## Summary

A fifth NuGet package, `<SERVER_ID>` (brief Decision 1; recommended `Eiva.Fletcher.Server`), carries
exactly the server shim `fletcher-c-abi-server` for `linux-x64` and `win-x64`, no managed assembly, and
depends on the base package `Eiva.Fletcher.Interop` (d10 §2 "Base package"; rul:536 (2), D21). BC-17
becomes one rule list in `CDP` over five packages, with Q7's default limits on the server shim and seven (fourteen as landed)
controls (six red today, one guard). The base package is unchanged: the edge shim only, under D-BIND-157's 12 MB.

## Design

**1. The package project** `dotnet/src/Fletcher.Server/Fletcher.Server.csproj` (in `Fletcher.slnx`, with
its generated `packages.lock.json`). `PackageId` = `<SERVER_ID>`; same version (`Directory.Build.props`),
TFMs, README (D-BIND-142, one text) and licence files as the others. **No build output in the package**
(`IncludeBuildOutput=false`, `IncludeSymbols=false`, `lib/<tfm>/_._` placeholders so each TFM group is
valid): the package is the shim and nothing else — "exactly the server shim" made structural, and no new
assembly, type or member exists (STOP list: no public surface beyond the package itself). A
`ProjectReference` to `Fletcher.Interop` yields the nuspec dependency on the base package. Packs
`$(FletcherServerShimLinuxX64)` → `runtimes/linux-x64/native/libfletcher-c-abi-server.so` and
`$(FletcherServerShimWinX64)` → `runtimes/win-x64/native/fletcher-c-abi-server.dll` — the layout rung 4's
loader already probes (`NL:256-284`). A `BeforeTargets="GenerateNuspec"` target refuses a pack **unless
both** are set and exist (stricter than Interop's "both or none": this package has no shim-less form).

**2. BC-17's one rule list** (replaces `CDP:74`, `:101-120`). One table, one comparison per row, keyed
by package id:

| Package | `runtimes/` entries, exactly | Fletcher-native dependencies, exactly | Ceiling |
|---|---|---|---|
| `Eiva.Fletcher.Interop` | the two edge shims | none | 12 MB (D-BIND-157) |
| `<SERVER_ID>` | the two server shims | `Eiva.Fletcher.Interop` | Q7 (below) |
| `Eiva.Fletcher` | none | `Eiva.Fletcher.Interop` | — |
| `Eiva.Fletcher.Model`, `Eiva.Fletcher.GatewayClient` | none | none | — |

"Fletcher-native dependencies" = the dependency ids ∩ {`Eiva.Fletcher`, `Eiva.Fletcher.Interop`,
`<SERVER_ID>`}, compared as a set **in each TFM group** (not their union), so a dependency is **required**
where listed and **refused** where not (audit note 2: the server's base dependency required, not skipped;
GatewayClient→server refused, BC-17 control 2; nothing depends on the server package, so a base-only
app never receives the server shim — the round purpose's second half). Errors name d10 L15 / BC-17;
"which only Eiva.Fletcher.Interop may" (`CDP:113`) is deleted (audit note 1, rul:337 (1)). **The server
package's every entry is on an allow-list**: NuGet's own metadata (`*.nuspec`, `[Content_Types].xml`,
`_rels/`, `package/`), the four root files (LICENSE, COPYING, THIRD-PARTY-LICENSES.txt, README.md),
`lib/<tfm>/_._` per TFM, and its two shims; anything else, anywhere, is refused. The existing
per-package checks (licences, README, commit, probe) run over all five (audit note 3).

**3. Each native package carries its own shim's notice** (audit note 4). `CDP`: the server package's
`THIRD-PARTY-LICENSES.txt` must list `arrow` (the bullet the graph-derived notice writes); **every other
package's must not** (they carry the edge notice, d9 L8). `scripts/check_third_party_notice.sh` gains the
nanoarrow-style rule for Arrow C++ (where `arrow` is listed, Arrow's Apache NOTICE text must be in it), and
the pack job calls it on the staged server notice with the edge list (`CI:475-476`) plus `arrow`.

**4. Q7's default on the server shim** (d10 Q7, standing per rul:566). Same DLL allow-list (D-BIND-148)
and the **same** floors (D-BIND-149, `shim_floors.env`): `scripts/check_shim_dependencies.sh` runs on
the server shim on both legs, unchanged. The Linux server copy is stripped exactly as the edge copy
(`CI:161-169`, the exports-unchanged check looped over both), and the server test leg then loads the
stripped copy, so the tested file is the shipped file. **Ceiling** (Q7 text: "first measured pack plus
the same margin D-BIND-157 left"; that margin is 12 − 7.7 = 4.3 MB): `SERVER_CEILING` = first measured
server pack + 4.3 MB, rounded up to a whole MB, with the formula, the bytes, image and commit beside it.
The measured artefact is the devcontainer pack (`fletcher-build:latest`, CI's image; Linux copy stripped);
CI re-confirms it without gating close and changes it only if its bytes round to another MB (C2-1). Overridable like `INTEROP_CEILING` (`CDP:38-39`).

**5. The relinking route covers the server shim** (D-BIND-140, -173, tier 2). Each CI leg also writes
the server graph's notice and lockfile into `dotnet/native/<rid>/server/`
(`conan install … -o "fletcher-c-abi/*:variant=server"`), checked by `scripts/check_conan_lockfile.sh`
given a new optional `server` argument (exports `arrow-bridge`, `pubsub-arrow` too; `-o &:variant=server`).
The pack job compares the two legs' server notices as it does the edge ones (`CI:462-466`), stages a
second licence dir, packs `<SERVER_ID>` with it, and puts `fletcher-shim-server-<rid>.conan.lock` beside
the packages; `CDP` requires them. `README.package.md` gains the server route (docs).

**6. The lists grow to five**: `cd.dotnet.yml` asset check (server: `.nupkg` and no `.snupkg`; its
lockfiles) and push order (Interop, then server, then the rest); `assemble_preview_bundle.sh:33`;
`ci.pr.yml`'s path filter gains the controls script. The preview consumer is rung 12's (audit note 5).
**Error paths:** every check is a pack-time refusal naming package, rule and source; no runtime code changes.

## Corner cases forbidden

- **A server package without both shims** — refused at pack (rung 2, the csproj target).
- **Either shim in the other's package, a probe shim, a third native file** — refused: exact-set rows.
- **Anything else in the server package, anywhere (an assembly, a native file under `lib/`, `build/`, the
  root)** — unrepresentable (no build output) and refused (the entry allow-list).
- **A server package not depending on the base, or anything depending on the server package, in any
  TFM group** — refused: exact per-group sets. A base-only app cannot pull in the server shim.
- **The edge notice in the server package, or an `arrow` notice in any other package** — refused (§3).
- **Linux and Windows server shims from different third-party graphs** — refused (notice comparison).
- **A server shim loading a DLL off the list, or with other floors** — refused; owner stop (Q7).
- **A stripped server shim with changed exports** — refused (looped check).
- **Server and base packages of different ABI versions in one app** — refused at load by the existing
  handshake (d10 L1); no new code. A **same-ABI** version skew loads without a signal: lawful (L1 judges
  compatibility by ABI version), not refused. **Handled residue: none.**

## Premises and stop conditions

- **P1 (Q7).** The server shim passes D-BIND-148's list and D-BIND-149's floors as they stand. **STOP-AND-ASK**
  if not (Arrow may raise glibc via `__isoc23_*`): report it; never widen a list or edit `shim_floors.env`.
- **P2.** ConanCenter's `arrow` package ships its `NOTICE.txt` under `licenses/`, so the deployer copies
  it. **STOP-AND-ASK** if not: completing a notice is D-BIND-152/-166 work, not this stage's.
- **P3.** NuGet packs a no-assembly package (`_._` per TFM, a dependency group each) with no warning.
  **STOP and report** if it needs an assembly: an empty assembly is an artefact users load, not a fallback.
- **P4.** Both legs' server graphs list the same libraries (else **STOP-AND-ASK**: one notice per
  package); rung 4's loader finds the server file in `runtimes/<rid>/native/` (built, T43).

## Forcing-test mapping

| Test | Green by | Red before, for the right reason |
|---|---|---|
| `CDP` over the five packed packages (rung 5, external) | §1 package + §2 rows + §3 notice + §4 ceiling + §5 lockfiles | `<SERVER_ID> was not packed` (today's script knows four ids) |
| `check_shim_dependencies.sh <server shim>`, both legs (extends T49) | §4 | not run on the server shim today |
| `scripts/test_check_dotnet_packages.py <folder>` — controls on copies of the packed folder: C1 `runtimes/` entry injected into `Eiva.Fletcher`; C2 GatewayClient→server dependency; C3 server package + an extra native file; C4 server without its Interop dependency; C5 server carrying the edge notice; C6 `Eiva.Fletcher`→server dependency; C7 `SERVER_CEILING=1` (must fail naming the ceiling). Each must fail `CDP` with its rule's message; the unmutated folder must pass. | §2, §3, §4 | C2-C7 red today (no server id, regex `CDP:118` blind to it); **C1 is a guard**, green both ways (T48's lawful half, re-anchored) |

The controls run in the pack job after "Check the packages", so they stay guards. Re-anchors T48;
extends T49; no test deleted (no subject is forbidden; `CDP:113`'s clause is wording, not a subject).

## Risks / Unknowns

- **Size vs plan.** Estimate **+210 / −55, net ≈ +155** (range +115…+185), under 250; 10 production
  files against the plan's 5-7 (several one-line list edits). **No split.** If it overruns: **5a** = §1-§4,
  §6; **5b** = §5 (server lockfiles, `check_conan_lockfile.sh` variant, README route), a declared one-stage
  gap in the relinking route; nothing ships (rul:278).
- **Tier 2 behind tier 1** (report, fix for owner approval — brief Decision 2): D-BIND-134, -139, -142
  ("four packages"), -140 (one shim's route) and -173 (one lockfile per platform) predate rul:337 (1).
  Suggested: annotate each "extended by D10C-5 to five packages / two shims, rul:337 (1)", text kept.
- **D-BIND-175** (not wired yet) covers managed assemblies; the server package has none, so its wiring
  must exclude it. **nuget.org**: a new id under Trusted Publishing is unproven until the first release.
- README batch prose (rungs 7, 12) and the PreviewConsumer re-anchor (`Program.cs:71-79`, rung 12) are
  not this stage's. No coexistence window opens. Evidence: Linux local, Windows by hand, CI on the draft PR.

## Files-to-touch

Production (counted): `dotnet/src/Fletcher.Server/Fletcher.Server.csproj` (new, +40) ·
`dotnet/Fletcher.slnx` (+1) · `scripts/check_dotnet_packages.sh` (+73/−25) ·
`scripts/check_third_party_notice.sh` (+4) · `scripts/check_conan_lockfile.sh` (+10/−4) ·
`.github/workflows/ci.dotnet.yml` (+62/−14) · `.github/workflows/cd.dotnet.yml` (+10/−5) ·
`.github/workflows/ci.pr.yml` (+1) · `scripts/assemble_preview_bundle.sh` (+2/−2) ·
`dotnet/src/Fletcher.Interop/Fletcher.Interop.csproj` (comment :6-10, +5/−5).
Generated: `dotnet/src/Fletcher.Server/packages.lock.json`. Tests (not counted):
`scripts/test_check_dotnet_packages.py` (new, ~+90). Docs (listed): `dotnet/README.package.md` (table,
native section, server route, notice), `dotnet/Directory.Build.props:6` and `.targets:58` ("four"), `check_shim_dependencies.sh:5`
header (F15), d10 L15 mark + "Holds at" (F23) + Findings (a)1 closed, `reg` BC-17 mark, the BIND annotations (Decision 2).

## Files-to-delete

- `CDP:112-114` "only Eiva.Fletcher.Interop may" branch and `:118-120` regex rule → replaced by §2's
  rule list (same lawful subject, re-anchored). `Fletcher.Interop.csproj:6-10` "the ONLY package …" →
  re-worded to L15. No file, type or test is deleted: the ruling requires a package; only wording retires.
