# D10C-5: step 0b test-authority audit (MODE=new-test), base 3ae1e8d

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE control. No tier-1 disagreement. Nothing schedules a forbidden test to return.
**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **613 lines, 46 `## ` entries; the last is `## 2026-11-16 — D10C-5: the server package's id; the BIND packaging annotations` (:605)**.
**Yardstick:** d10 L15 ("Native code ships only in shim packages, one shim per package … Every other managed package is asset-free"), §2 "Base package" = `Eiva.Fletcher.Interop`, Q7 default (stands, :566). BC-17, ladder rung 5. Rulings :337 (1), :363 (4) as refined by :575 (1), :536 (2) "the server package depends on the base one", :566 purpose, :605 (1) "`Eiva.Fletcher.Server` … carries only the server shim and depends on `Eiva.Fletcher.Interop`".
**Scope:** `scripts/test_check_dotnet_packages.py` (untracked, 237 lines): the baseline, C1-C7, and the helpers `mutate`, `add_dependency`, `drop_dependency`, `run_check`, `refusal`.

| Control | Class | Ground |
|---|---|---|
| baseline (the folder as packed passes, no `::error::`) | LAWFUL | It accepts only lawful facts: server→Interop (:605 (1)) and Eiva.Fletcher→Interop (d10 §2). |
| C1 runtimes/ entry in Eiva.Fletcher | LAWFUL | L15 "asset-free". Needles are the package and the path, with no wording and no cite, so the deleted `CDP:113` clause is not pinned. |
| C2 GatewayClient→server | LAWFUL | BC-17 "GatewayClient depends on neither". |
| C3 a third native file in the server package | LAWFUL | BC-17 "exactly their own two shims"; L15. |
| C4 server without its Interop dependency | LAWFUL | :536 (2) and :605 (1): the dependency is required, not just allowed. |
| C6 Eiva.Fletcher→server | LAWFUL | :363 (4)/:575 (1): the server shim loads only if the app chose the server package; :566 says a base-only app stays Arrow-free. |
| C7 `SERVER_CEILING=1` | LAWFUL | d10 Q7 ceiling. The knob is the design's (§4), copied from `INTEROP_CEILING`; today's script does not have it. |
| C5 the edge notice in the server package | UNMOORED (tier 2) | No law or ruling covers notices (also 0a). It rests on D-BIND-140/-152 through 0a note 4 and design-review DEBT-2. It is a pack-time licence check, not a new VALUE subject or surface, so it is not a stop. It rightly cites neither L15 nor BC-17. |

**Law (b):** met. Every control runs the bash script as a subprocess over a fresh copy of the folder. Nothing is sourced, imported or edited in place.
**Pinning the current script:** none. C2-C4 and C6 require an `L15`/`BC-17` cite, which the design requires (§2 "Errors name d10 L15 / BC-17") and today's script never prints. One constraint the implementer should know: each control needs the breakage named on **one** `::error::` line. Today's `CDP:110` splices a multi-line list into its message, so C3's extra path must be printed on the error line itself, not on a continuation line.
**Vacuity:** none found. The baseline gate stops a broken folder. `<dir>` masking stops a scratch path from matching a needle. Package-file needles do not collide: `Eiva.Fletcher.<v>.nupkg` is not a substring of `…Server.<v>.nupkg`. Stripping `*_CEILING` from the environment rules out C7 passing on an inherited value. Today, with no server .nupkg, the test exits at the five-package pre-check; with a stand-in, C2-C7 are each red for their own reason (the `:118` regex cannot see `.Server"`; `Eiva.Fletcher` is exempt; the server id is not in the loop).
**C1, "red control" (BC-17) vs "guard" (design):** both are right, because the two documents mean different things by the words. BC-17's "red control" is a negative control, a mutation that must turn the check red. The design's "guard" is about red-first status: C1 passes today (`CDP:112-113`), so it cannot be this stage's red. The design's label is correct for 0b. The register needs no amendment; it may say "negative" when its mark is updated.
**Laws with no control (gaps, not findings):** "`arrow` absent from every other package's notice" (design §3); a dependency missing or extra in only **one** TFM group, because `add_`/`drop_dependency` change every group at once.
