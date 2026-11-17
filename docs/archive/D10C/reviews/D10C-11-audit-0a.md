# D10C-11 step 0a audit (corpus): rung 12, the purpose from packages; T46

**Verdict: CLEAR** (with re-anchor conditions below). No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test in scope; no tier-1 disagreement.
**Proof of read:** `plans/RIX-rulings.md` 684 lines, 54 `## ` entries, last "## 2026-11-17 — Fletcher Linking Exception: legal approval; pack it" (:677).
**Yardstick:** d10 §1, L5 ("never the edge shim in place of a present server shim"), L6 ("A batched call on the edge build fails with a named not-supported exception that names the server package"), L7 ("Everything but the batched subscribe works on the edge shim"), L15; BC-17; rulings :551 (1), :566 (purpose), :575 (1)/(2), :605 (1), :650 (2), :677. Spec/BIND tier 2 for context only.

| Test (file:line) | Class | Action / why |
|---|---|---|
| T46 `NativeBatch`, `Program.cs:68,129-159` (base refs only, `PreviewConsumer.csproj:28-30`); declared red, :650 (2) | LEGITIMATE-BUT-ADJACENT | **RE-ANCHOR** (ordered by :650 (2)). Subject "a packaged consumer gets a batch" is the purpose (:566); its base-only vehicle is what L6/:551 (1) forbid. Server config: rows expected. ⚠ Base-only config must NOT keep a rows-expected arm (that would be FORBIDDEN, L6); it asserts `NotSupported`, shim text plus one sentence naming `Eiva.Fletcher.Server` (:575 (2)). Today it throws uncaught, so the later arms never run. |
| Shim-identity check `Program.cs:71-81` | LAWFUL (base config), LEGITIMATE-BUT-ADJACENT (server) | **RE-ANCHOR per config**: the server config expects `fletcher-c-abi-server` from `Eiva.Fletcher.Server`'s `runtimes/<rid>/native/` and the edge file NOT loaded (L5 "never both"). ⚠ A check that accepts either file would be FORBIDDEN-BY-TOLERANCE (L5). |
| `minimal.sh:36-47` (OK required of the base-only build) | LEGITIMATE-BUT-ADJACENT | **RE-ANCHOR with T46**: it inherits T46's base-only batch arm. Below-floor arm `:49-64`: LAWFUL. If it runs on the server build, it must name the server file and must not fall back to the edge shim (L5, :575 (1)). |
| `run.sh` (one project), csproj snippet `csproj-references` `:26-35` | LAWFUL | The harness needs a second configuration (rung 12, "two configurations"). The base reference list is L7's case. |
| `NativeRows`, `NativeFastDds`, `GatewayRows/Batch`, `GrpcConsumer` | LAWFUL | L7 per row on edge; the gateway is domain 5's (d10 §1 Refuses). Untouched. |
| `assemble_preview_bundle.sh:34-36` (five packages incl. Server) | LAWFUL | L15, :605 (1). |
| `assemble_preview_bundle.sh:27-28,48-52` (bundle-root LICENSE/COPYING only) | UNMOORED | Not a violation. :677 puts the exception in "the packages D-BIND-175 names" and Releases; previews are not Releases (:624 (1)), and the packages already carry it (`check_dotnet_packages.sh:149-155`). No law names the bundle root. |
| `check_dotnet_packages.sh:118-203` + `test_check_dotnet_packages.py` (C1-C16), `check_readme_snippets.py` | LAWFUL | BC-17 / L15. The control "`Eiva.Fletcher` depending on Server fails" guards L7. C16 (no exception in the server package) matches D-BIND-175 and :677. |

**Scheduled restoration:** none forbidden. :650 (2), ladder :57/:60 and the pre-round audit :390 re-point T46 to the server package; none of them restores a base-only batch assertion.
**Laws with no test:** no from-packages test exists for L6 (base-only gives the named refusal), L7 (base-only output holds no server shim; package-level only, via BC-17's control) or L5 (server preferred when present). Rung 12 creates them. L7's mark still reads "designed-not-built for the switch".
**Stale prose (not tests; already rung 12's docs list):** `README.package.md:150-155` says handlers run concurrently and a schema-less subscription drops for life, contradicting L13, :551 (4) and :258 (3). `preview-bundle-consumer/README.md:26-30` names Interop's shim only.
