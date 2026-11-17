# D10C-5 compliance review (step 4a), cycle 1: ISSUES (4 blocking) · cycle 2: PASS

Base `3ae1e8d` vs the working tree (LICENSE-EXCEPTION excluded); design cycle 2. **Proof of read:** `plans/RIX-rulings.md`, top to bottom:
**613 lines, 46 `## ` entries; last `## 2026-11-16 — D10C-5: the server package's id; the BIND packaging annotations` (:605)**. No tier-1 collision.

## Blocking
**F1. Ruling :605 (2) is not applied exactly.** It names D-BIND-134, -139, -140, -142 and **-173**. The fifth annotation sits at
`BIND-locked-decisions.md:4108`, inside **D-BIND-176** (:4065), so D-BIND-173 (:3977-4017) is left unannotated. *Fix:* move the line to
the end of D-BIND-173. Annotating D-BIND-176 as well would need the owner's approval, because :605 (2) does not cover it.

**F2. The server relinking route in the README fails at its first command** (design §5, D-BIND-140). `README.package.md:298` passes
`-o "&:variant=server"` to every `conan create`, but only `c-abi/conanfile.py:42` declares `variant`. Conan 2.26.2 (scratch home) stops
with `ERROR: option 'variant' doesn't exist`. CI never runs this route; it passes the option to c-abi only (`ci.dotnet.yml:139`).
*Fix:* use `-o "fletcher-c-abi/*:variant=server"` (verified: the other recipes ignore it), then rehearse once from an empty Conan home.

**F3. P4 is breached, not "half checked", and the design makes that STOP-AND-ASK.** I ran `conan graph info c-abi -o &:variant=server`
with both profiles. Linux has 16 host third-party packages, Windows 15: only Linux has `libbacktrace/cci.20210118`, a host, visible,
non-test dependency of `boost/1.90.0`. The deployer lists such dependencies (`third_party_licenses.py:125`), so the two server notices
differ and the pack job's comparison (`ci.dotnet.yml:497-502`) fails on the first CI run. Caveat: today's ConanCenter, not the stage's
lockfile; the dependency comes from boost's recipe structure. *Fix:* ask the owner (one notice per RID, a union notice, or no backtrace
in the server graph); record P4 as breached.

**F4. Ruling :605 is not ingested in d10 §2, which its "Applies to" names.** `d10:83` still says `[designed-not-built]` and "Its NuGet
id is the packaging stage's choice", and `d10:50` has the same mark. *Fix:* name `Eiva.Fletcher.Server` (:605 (1)) and mark both
`[built]` with L15's qualifier.

## Non-blocking
**N1. Wording that survived** (not in the design's lists, so fixing it is not scope creep): `d10:102-104` (strike it as (a)1 was);
`docs/technology-decisions.md:141` (annotate it, do not rewrite it); the `ci.dotnet.yml:553-555` comment ("nothing else … depends on a
package that does", which is false); "four packages" at `README.md:425` and `docs/component-diagram.md:324`.
**N2.** `check_dotnet_packages.sh:444`: with no `targetFramework` group, the fallback prints "none", so a flat dependency on the server
package passes. Fix: report it on a `(no group)` line.
**N3.** D-BIND-14′'s "Adding a fourth package → STOP-AND-ASK" is not annotated, although :337 (1) answered it. Annotating it needs the owner's approval.

## Checked, no finding
The package (id, no assembly, the Interop dependency in each group); BC-17/L15 (exact `runtimes/` sets, the Interop dependency required, any
dependency on the server package refused, `arrow` only in the server notice); Q7 (12,848,400 + 4,300,000, rounded up to 18,000,000); the
strip, dependency check and lockfiles on both legs; the cd assets and push order; Files-to-delete done; the `dotnet/README.md` edit is
cleanup; no test retired; the `[built]` marks say "CI unrun" (:408 (2)); no edit to d7 or d9.
RECORD: net +199 against the declared +115…+185 (under 250). RECORD: `dotnet/README.md:36` "All five … pin `Apache.Arrow`" was false.

## Cycle 2 — PASS (no blocker; the +260 overrun is the owner's open question and is not ruled here)
**Proof of read:** I re-read the ledger: **641 lines (`wc -l`), 49 `## ` entries; last `## 2026-11-16 — Fletcher Linking Exception:
friend assemblies (m6)` (:636)**. Lines :1-613 are unchanged (the ledger diff since `3ae1e8d` only adds lines); :615 is this stage's
ruling, and :624 and :636 concern LICENSE-EXCEPTION. No collision: :636's "csproj (unchanged)" is about InternalsVisibleTo, which this
stage does not touch.
- **Resolved:** F1 (the annotation ends D-BIND-173 at :4018; none on D-BIND-176). F2 (`README.package.md:298` uses the
  `fletcher-c-abi/*` pattern; the route was rehearsed green from an empty Conan home). F3 per :615 (`check_notice_covers.sh` ships the
  platform notice that contains the other and refuses when neither does, so no partial notice ships; the edge comparison stays identical;
  C14 red first; P4 recorded as breached, then resolved). F4 (`d10:50`, `:83`). N1, except `architecture-overview.md:77`. N2 (a
  loose-dependency line; a nuspec with no group is refused; C12, C13). Both RECORD items. 4b S2–S4 (C10, C11) stay within BC-17/L15.
- **Open, non-blocking:** (a) :615's "Applies to" names D-BIND-134/-140, which carry no union note, although D-BIND-134 says the notice
  "lists the libraries linked into the shim" and Windows' notice now lists libbacktrace. Suggested: "*the server package's notice is
  the union of both platforms' graphs (:615)*", here or by the PM at close. (b) N3 is still with the owner. (c) `architecture-overview.md:77`
  still says "four NuGet packages".
RECORD: d10 L15 "Holds at" cites `check_dotnet_packages.sh:113-176`; the rule loop is now :118-203.
