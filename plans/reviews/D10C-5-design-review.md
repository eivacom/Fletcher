# D10C-5 design review — cycle 1 **NEEDS-REWORK** (1 BLOCKER, 4 DEBT)

## Cycle 1 (`a32f738`) — NEEDS-REWORK
Proof of read: `plans/RIX-rulings.md` read top to bottom: **603 lines, 45 `## ` entries; the last is `## 2026-11-15 — D10C
ladder: the domain-7 port tests` (:595)**. No tier-1 collision found.

- **B1 — BLOCKER (deviates from an owner-confirmed default; STOP-AND-ASK only if the ratio is kept).** d10 Q7's default, which
  stands by :566, sets the ceiling "from the first measured pack **plus** the same margin D-BIND-157 left" (ladder rung 5:
  "+ D-BIND-157's margin"), and that margin is 12 − 7.7 = 4.3 MB. Design §4 (:59-62) multiplies by 12/7.7 instead, and rejects
  4.3 MB by citing an owner preference no text records ("a tighter wire than the owner chose"). The readings drift apart with
  size: at a 25 MB pack the ceiling is 39 MB against 30 MB, so about 3× as much growth passes unnoticed. The brief never states
  the formula, and Q7 brings the owner only list or floor breaches, so the owner's tripwire is loosened unseen.
  **Acceptable fix (cheapest):** follow the text. `SERVER_CEILING` = first measured pack + 4.3 MB, rounded up to a whole MB,
  formula beside it; delete "Relative, not absolute". No owner question. Keeping ×12/7.7 instead takes one brief line putting
  that reading to the owner (the default "stands unless the owner objects"). A forbid does not apply.

**DEBT** (register: `plans/reviews/design-debt.md`, D10C-5). None of these loops the design.
- **DEBT-1 (step 3) Name the artefact that sets the ceiling:** the first CI pack job's server `.nupkg`, Linux copy stripped,
  with its bytes and run ID beside the constant (an unstripped local `.so` inflates it: the edge pack was 8.6 vs 7.7 MB). Add
  C7: `SERVER_CEILING` overridable like `INTEROP_CEILING` (`CDP:38-39`); a 1-byte ceiling must fail, naming the ceiling.
- **DEBT-2 (step 3) "Exactly the server shim … or holding anything else" (design :23, brief :19-20) is checked only under
  `runtimes/` and for `lib/**/*.dll`.** A native file at `lib/<tfm>/x.so`, under `build/` or at the root passes. Forbid it
  with an allow-list of the server package's entries (nupkg metadata, four root files, `lib/<tfm>/_._`, two shims), cheaper
  than listing forbidden places. Compare dependency sets per TFM group, not their union. Apply "must not list `arrow`" to
  every package but the server, not to Interop alone.
- **DEBT-3 (step 3)** The pack job must call `check_third_party_notice.sh` on the server notice with the edge list
  (`CI:475-476`) plus `arrow`, or §3's Arrow NOTICE rule never runs. The design names the rule but not the call.
- **DEBT-4 (PM, brief :23) "Mismatched server/base versions are refused at load" overclaims.** Only different *ABI* versions
  are (design :88 is right). Same-ABI skew (`Eiva.Fletcher` 0.6.1, server package 0.6.0) loads the older server shim with no
  signal: compatible under L1, but not "refused". Reword. Optional forbid: an exact-version Interop dependency refuses it at restore.

**Checked, no finding:**
- **The rule list is BC-17 plus L15, exactly.** Native files: Interop = two edge shims, server = two server shims, the other
  three none ("asset-free"). Dependencies are compared as a set ∩ {Eiva.Fletcher, Interop, server}: the server's Interop
  dependency is *required* (audit note 2), and any dependency on the server package is refused, `Eiva.Fletcher`'s included
  (C6), so a base-only app never receives the server shim. GatewayClient "none" = BC-17's "neither" = today's `CDP:118`.
- **Q7 sets its ceiling by measuring itself:** as the default prescribes and :566 accepted. It is a real bound from the second
  pack on, once DEBT-1 pins the first. Same allow-list and floors with P1's STOP (D-BIND-148/-149's terms); base keeps D-BIND-157.
- **NuGet.** The `_._` placeholders, one per TFM, avoid NU5128. Per-TFM groups are right (a TFM-less group would advertise
  frameworks the Interop dependency cannot serve). P3 stops rather than ship an empty assembly. No `.snupkg` agrees with cd.
- **Rebuild route.** Server notice and lockfile per RID, legs compared, release assets. The `variant=server` graph check's new
  exports are `fletcher-*` (verified), so `conan lock remove` strips them. D-BIND-175: no managed code, so stays LGPL-only. Right.
- **Controls.** All six drive CDP as a subprocess over mutated copies of the packed folder. Each must fail with its own rule's
  message after the unmutated folder passes, so none is vacuous; C1 is a declared guard. A test, per `scripts/test_check_*.py`.
- **7a/7b.** Design 149/150 and brief 55/55: compressed, not padded; the only casualty is B1's formula. Priced independently:
  csproj ~40, CDP ~+55/−25, `ci.dotnet.yml` ~+60/−14, the other six ~+30/−16, **net ≈ +130…175 against 250. No split.**
  An overflow takes the ladder's 5a/5b; the design's fallback cut (5b = lockfiles, not Q7 limits) is the PM's call if ever needed.
- **Tree claims verified:** `CDP:74, :101-120, :113, :118`; `CI:161-169, :462-466`; `NL:256-284` probes
  `runtimes/<rid>/native/`; `assemble_preview_bundle.sh:33`; cd `:107, :174`. No tree file names any candidate id.
- **Decision 1 is genuinely open** (d10 §2: "Its NuGet id is the packaging stage's choice"; ladder :39). Putting it to the owner
  is proportionate: a nuget.org id is permanent, and D29's sentence shows it to users. Every option has a user line and an
  architecture line, and (a) is grounded in the purpose and d10's term. Agree.
- **Decision 2 belongs to the owner:** the authority order gives tier-2 corrections to the owner to approve (precedents :383 (2),
  :575 (7)). Only the "now or at close" timing is bookkeeping. Both lines are present. Agree with (a).
