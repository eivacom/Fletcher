# D10C-5 compliance review (step 4a), cycle 1: ISSUES (4 blocking)

Base `3ae1e8d` vs the working tree (LICENSE-EXCEPTION excluded); design cycle 2. **Proof of read:** `plans/RIX-rulings.md`, top to bottom:
**613 lines, 46 `## ` entries; last `## 2026-11-16 — D10C-5: the server package's id; the BIND packaging annotations` (:605)**. No tier-1 collision.

## Blocking
**F1. Ruling :605 (2) is not applied exactly.** The ruling names D-BIND-134, -139, -140, -142 and **-173**. The fifth annotation sits
at `BIND-locked-decisions.md:4108`, inside **D-BIND-176** (:4065), not D-BIND-173 (:3977-4017). D-BIND-173 is left unannotated, and
D-BIND-176 carries an annotation no ruling covers. *Fix:* move the line to the end of D-BIND-173. Annotating D-BIND-176 too (its push
order, "all five IDs were free") would be a separate tier-2 correction for the owner to approve, because :605 (2) does not cover it.

**F2. The server relinking route in the README fails at its first command** (design §5, D-BIND-140). `README.package.md:298` passes
`-o "&:variant=server"` to *every* `conan create`, but only `c-abi/conanfile.py:42` declares `variant`. With Conan 2.26.2 in a scratch
home, `&:variant=server` on a recipe without the option gives `ERROR: option 'variant' doesn't exist`, so `conan create core` stops.
CI never runs this route: it passes the option to c-abi only (`ci.dotnet.yml:139`). *Fix:* use `-o "fletcher-c-abi/*:variant=server"`
on every command (verified: other recipes ignore it), or pass the option to c-abi only. Then rehearse the route once from an empty
Conan home, as D-BIND-173 did for the edge route.

**F3. P4 is breached, not "half checked", and the design makes that STOP-AND-ASK.** I resolved `conan graph info c-abi
-o &:variant=server` with both profiles in a scratch home. Linux has 16 host third-party packages and Windows has 15: only Linux has
`libbacktrace/cci.20210118`, which `boost/1.90.0` requires as a host, visible, non-test, non-skipped dependency. The deployer lists
exactly such dependencies (`third_party_licenses.py:125`), so the two server notices differ. The pack job's comparison
(`ci.dotnet.yml:497-502`) will therefore fail on the first CI run, before "Check the packages". Caveat: I used today's ConanCenter
without the stage's lockfile, but the Linux-only backtrace dependency comes from the structure of boost's recipe. *Fix:* ask the owner
to choose between one notice per RID, a union notice, or dropping boost's backtrace from the server graph (a recipe or profile change,
outside this stage). Record P4 as breached in the evidence row, not as "not run".

**F4. Ruling :605 is not ingested in d10 §2, which its "Applies to" names.** `d10:83` still reads `[designed-not-built]` and "Its
NuGet id is the packaging stage's choice", and `d10:50` (§1) carries the same mark. *Fix:* name `Eiva.Fletcher.Server` (ledger
:605 (1)) and mark both places `[built]` with the same qualifier as L15.

## Non-blocking
**N1. Wording that survived** (none of it in the design's lists, so fixing it is not scope creep). `d10:102-104`'s naming finding
("Interop is the only package with native assets … stated in code and checked"): strike it as (a)1 was. `docs/technology-decisions.md:141`
("Four NuGet packages … the only package with native assets"): annotate it (by :337 (1), :605), do not rewrite it. `ci.dotnet.yml:553-555`
("nothing else … depends on a package that does") is false, since Eiva.Fletcher and the server depend on Interop: restate it as BC-17's
rule. "Four packages" survives at `README.md:425` and `docs/component-diagram.md:324`; `preview-bundle-consumer/README.md:53` is rung 12's.

**N2. One shape gets past the per-group dependency check.** `check_dotnet_packages.sh:444`: a nuspec with no `targetFramework`
group falls back to "none" without reading any flat or TFM-less `<dependency>`. A flat dependency on the server package from another
package therefore passes, which breaks "refused in any TFM group". SDK packs never write that shape, so the risk is low. *Fix:* report
such dependencies on a `(no group)` line.

**N3. D-BIND-14′ is out of step with tier 1.** Its "Adding a fourth package → STOP-AND-ASK" (amended to four by D-BIND-74) was
answered by :337 (1) but is not annotated. Suggested annotation, for the owner to approve: "a fifth, native-only package by :337 (1), :605 (1)".

## Checked, no finding
The package (`Eiva.Fletcher.Server`, no assembly, `_._` per TFM, depends on Interop in each group). BC-17/L15 (exact `runtimes/`
sets for all five; the server's Interop dependency required; any dependency on the server package refused; `arrow` only in the
server notice). The Q7 ceiling (12,848,400 + 4,300,000 = 17,148,400, rounded up to 18,000,000, per d10 Q7 and :566; provenance per
C2-1). The server shim in CI and CD (strip loop, stripped copy tested, `check_shim_dependencies.sh` on both legs, a server lockfile
per leg with `check_conan_lockfile.sh server`, the cd asset list, no `.snupkg`, push order). Files-to-delete (`CDP:112-114`, `:118-120`
gone; `Fletcher.Interop.csproj:6-10` states L15). The unlisted `dotnet/README.md` edit, which is "four packages" cleanup, not a
breach. Tests: none retired; C1 re-anchored as a guard; C8 and C9 close the 0b audit's gaps. The `[built]` marks carry "CI unrun",
following :408 (2), so they are honest (F3 makes the first CI run red). No edit to d7, d9 or their code.
## RECORD
- RECORD: counted production is +217/−75 plus the new 57-line csproj, net +199: above the declared +115…+185, under 250.
- RECORD: `dotnet/README.md:36` "All five … pin `Apache.Arrow` exactly" is false for the server package (and already was for Interop).
