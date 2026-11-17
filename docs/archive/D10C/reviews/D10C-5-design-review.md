# D10C-5 design review — cycle 1 NEEDS-REWORK (1 BLOCKER) · cycle 2 **APPROVE-WITH-DEBT(1)**

## Cycle 1 (`a32f738`) — NEEDS-REWORK (1 BLOCKER, 4 DEBT)
Proof of read: `plans/RIX-rulings.md` read top to bottom: **603 lines, 45 `## ` entries; the last is `## 2026-11-15 — D10C
ladder: the domain-7 port tests` (:595)**. No tier-1 collision found.
- **B1 — BLOCKER (deviates from an owner-confirmed default).** d10 Q7's default, which stands by :566, reads "first measured
  pack **plus** the same margin D-BIND-157 left", i.e. + 4.3 MB (12 − 7.7). §4 multiplied by 12/7.7 instead, on an owner
  preference no text records. At a 25 MB pack that is a 39 MB ceiling against 30 MB, and the brief did not show it.
  Fix: follow the text (+ 4.3 MB, rounded up to a whole MB), or put the ratio reading to the owner in the brief.
- **DEBT-1** name the measured artefact, and add C7 with `SERVER_CEILING` overridable. **DEBT-2** check the server package's
  entries against an allow-list, compare dependencies per TFM group, and refuse an `arrow` entry in every package but the
  server. **DEBT-3** the pack job calls `check_third_party_notice.sh` on the server notice. **DEBT-4** the brief's "refused at
  load" is true only for different ABI versions.
- **Checked, no finding:**
  - The BC-17/L15 rule list: the server package must depend on Interop, nothing may depend on the server package
    (`Eiva.Fletcher` included), and the other packages carry no native files.
  - NuGet: one `_._` per TFM avoids NU5128, per-TFM groups beat a TFM-less group, and P3 stops rather than ship an empty
    assembly.
  - The relinking route and the D-BIND-175 note.
  - The six controls run from outside and none is vacuous; both documents are compressed, not padded.
  - Decision 1 is open (d10 §2, ladder :39) and proportionate to ask about, since an id is permanent; I agree with (a).
  - Decision 2 is the owner's, as a tier-2 correction (:383 (2), :575 (7)); I agree with (a).

## Cycle 2 (`ae0d368`) — **APPROVE-WITH-DEBT(1)**, no BLOCKER stands
Ledger re-checked: unchanged at 603 lines and 45 entries; the last is still :595.
**Resolved:**
- **B1:** §4 (:62-64) now sets `SERVER_CEILING` = first measured pack + 4.3 MB, rounded up to a whole MB, with the formula
  beside it, and the "relative" argument is gone. The brief (:8-9) states the limit to the owner.
- **DEBT-1:** C7 (`SERVER_CEILING=1` must fail, naming the ceiling) is in the forcing table, and the ceiling is overridable
  like `INTEROP_CEILING` (:66). Provenance is now C2-1 below.
- **DEBT-2:** every server-package entry is on an allow-list (:46-49); the dependency sets are compared per TFM group (:42);
  every package but the server must not list `arrow` (:53-54). Corner cases :85-89 match.
- **DEBT-3:** the pack job calls the notice check on the staged server notice, with the edge list plus `arrow` (:55-56).
- **DEBT-4:** design :93-95 and brief :24-25 now say same-ABI skew loads without a signal and is lawful under L1.

**7b re-priced:** `CDP` is now +73/−25 (allow-list ~8 lines, per-group parse ~10, `arrow` everywhere ~2, override ~2), plus
`ci.dotnet.yml` +62/−14. The declared net ≈ +155 (range +115…+185) holds; I put it at +155…+175 against 250. **No split.**

- **C2-1 — DEBT (PM, before the owner reads the brief): the ceiling waits on a CI run this stage cannot get.** §4 (:64-66)
  makes a local value provisional, "replaced from CI before close" (brief :9 "CI-measured", :49 "fixes the size limit"). But
  `ci.pr.yml:6-7` runs only on `pull_request`, and no PR exists mid-round: an unmeetable close condition, which gets waived
  quietly. Not a BLOCKER, because Q7 says "first measured pack", not "first CI pack". The devcontainer is CI's own image
  (`fletcher-build:latest`) and ran the pack job's commands green in D10C-4 (Interop 7,912,335 bytes, `D10C-evidence.md:15`),
  giving the shipped form: Linux stripped, Windows msvc194 Release. A growth tripwire needs that measurement and its provenance.
  **Acceptable fix:** set `SERVER_CEILING` at this stage from the devcontainer pack, with bytes, image and commit beside it
  (not "CI run ID"). CI re-confirms rather than gates: its first green pack job records bytes and run ID in the evidence
  ledger (with D10C-4 DEBT-5), and the constant changes only if CI's bytes round to a different whole MB. Strike "replaced
  from CI before close" (:65-66), "CI-measured" (brief :9) and "which also fixes the size limit" (brief :49).
- Decisions 1 and 2 are unchanged and my view stands. No new tier-1 or locked-decision deviation.
