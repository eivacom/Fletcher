# D9S-3 design review — cycle 1

**Verdict: APPROVE-WITH-DEBT(3).** No BLOCKER. No ruling, law or locked-decision deviation.

**Reviewed:** `plans/D9S-3-edge-arrow-free.md`, `plans/D9S-3-brief.md`, `scripts/check_edge_arrow_free.sh`
(uncommitted, 90 lines).
**Proof of read:** `plans/RIX-rulings.md` has 419 lines and 31 entries. The last heading is "2026-11-13 —
D9S-2 close: ceiling overrun accepted; CI evidence via the owner's draft PR". I read it top to bottom.
**Budget:** the design is 294 lines (≤300), the brief 56 (≤60), and the new public surface is 0. Implied
work: one script that already exists, about 16 YAML lines and doc marks. That is about +106 against the
250 ceiling, so no split is needed.
**Tree claims verified:**
- `c_abi_tests` links `arrow::arrow` (`c-abi/tests/CMakeLists.txt:36,77-78`).
- The probe block leaves the shim target's link untouched (`CMakeLists.txt:183-187` vs `:203-238`).
- `ci.dotnet.yml:155` strips the packaged `.so` afterwards.
- The citations `ci.c-abi.yml:29-32` and `:180-183` are as stated.
- Arrow is required only under `run_tests` (`conanfile.py:95-107`), and `run_tests` is removed from the
  package ID (`:109-110`).

## The six points

1. **Structural or a site list? Structural.** The check judges two outcomes, the resolved host graph and
   the names in the built artefact, and names no recipe or CMake line.
   - Any Conan route through the `arrow` node is red, including transitive ones.
   - A renamed or vendored Arrow, or a link by path, gets past the graph and is caught by the symbol
     half (fully on Linux).
   - The three package names and the namespace are L8's and BC-11's subject, not sites.
2. **Windows limit: honestly scoped.** The design says plainly that this is evidence, not proof, and
   names the covering nets.
   - "L12 forbids closing it" holds for `/DEBUG`, which changes the shipped image.
   - It overreads L12 for `/MAP`. L12's "no link" is "(L8)", meaning an Arrow link. A map leaves the
     artefact byte-identical, and ladder rung 3 offered "PDB/link map, at the architect's choice".
   - The decision itself (not closed here; covered by the graph and Linux) is lawful on the ladder's
     own terms. Only the reasoning needs rewording: **DEBT-1**.
3. **Controls: they can fail, and they run on every invocation.** Only the edge refusal skips them, and
   that refusal is itself `exit 1`.
   - The symbol control runs the same `scan` and `$AR` as step 2. Sound.
   - The graph control asserts the literal `fletcher-arrow-bridge`, not step 1's predicate
     `$ARROW_PKG`, and its "naming:" may print empty and still pass: **DEBT-3**.
4. **Can the check pass having read nothing? I found no path that does.**
   - Graph: errexit is off inside `$(graph_hosts)`, so only the explicit checks guard it, and they hold.
     The root guard catches wrong JSON or a wrong recipe. If the schema drifts and empties the host
     list, the graph control catches it, because it uses the same parser.
   - Symbols: the Fletcher-name guard refuses a stripped, non-ELF or undemangled listing. Mangled
     output has no `fletcher::`, so the guard fires before premise 2's control would.
   - `nm … || true` swallows nm's exit status. A partial listing would need a truncated packaged file
     that the suite had just loaded, which cannot happen. Not raised.
5. **Linux evidence plan: sound.** Premises 1–3 each have a stop condition, and a false premise 1 is
   refused rather than passed. So a green Linux run (guard met, both controls red by name) is evidence
   in itself. When stages close, the ledger should copy the run's two control lines and the "carries
   fletcher::" line.
6. **Duplication: none.** `locate_shim.sh` stays the single place that selects the edge build: CI calls
   it and the script takes a path.
   - The control's `find -print -quit` answers another question ("any binary that links Arrow") and
     measures nothing. F8 protects measurements, so it does not apply. This is acceptable and justified
     in the design.
   - `scan`'s dispatch by extension is not `table_of` (another tool, another question).

**D9S-4:** with premises (a), (b) and (c), the check stays green and meaningful, and nothing here has to
be undone. An Arrow header pulled into `fletcher-c-abi-impl` would need Arrow in the edge graph, so the
graph half would turn red, which is correct.

## DEBT

**DEBT-1 (PM, brief before the owner reads it).** Brief line 38 says L12 forbids a link map or PDB, and
the design's §Windows calls `/MAP` "a change to the edge build".
- `/DEBUG`: correct. It changes the shipped image.
- `/MAP`: overstated. The artefact stays byte-identical, L12's "no link" means an Arrow link (L8), and
  ladder rung 3 offered a link map at the architect's choice.
- Reword to: "not needed given Linux and the graph; Windows parity through `/MAP` is a later-round
  question." Otherwise the doc marks will record L12 as forbidding a link map.

**DEBT-2 (step 3, wiring).** Run the named check **before** the size ceilings on both legs.
- Today a large static Arrow link trips "Measure the packaged shim" (Windows) or the Linux ceiling's
  `exit 1` first. The by-name step never runs, so the culprit is never named, which defeats the stage's
  purpose.
- Fix: put the Windows step before "Measure"; put the Linux call right after `SHIM=` at `:207`. The
  size steps themselves stay unchanged (L12).
- The Linux comment sits inside `bash -c '…'`, so it must contain no apostrophe. The block's existing
  comments already avoid them.

**DEBT-3 (step 3, script).** Step 3 should judge with step 1's own predicate:
`ctl=$(grep -xE "$ARROW_PKG" <<<"$tests")`, required to name both `fletcher-arrow-bridge` and `arrow`
(`arrow` is L8's subject). Then a damaged `$ARROW_PKG` cannot leave step 1 vacuous while the control
stays green. This is a 1–2 line change.
