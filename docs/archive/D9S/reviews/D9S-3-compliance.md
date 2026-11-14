# D9S-3 — conformance review (step 4a), cycle 1

**Verdict: ISSUES(2, 0 blocking).** The implementation conforms to the design as amended by DEBT-1/2/3.
Two non-blocking findings follow, then RECORD lines.

**Diff:** `3dcfd69..HEAD` (`52a0103`). Production: `scripts/check_edge_arrow_free.sh` +92,
`.github/workflows/ci.c-abi.yml` +19, `.github/workflows/ci.pr.yml` +1. Docs: d9 and BC-11. Audit:
0b.
**Proof of read:** `plans/RIX-rulings.md` has 419 lines and 31 entries. The last heading is
"2026-11-13 — D9S-2 close: ceiling overrun accepted; CI evidence via the owner's draft PR". I read it
top to bottom. No tier-1 collision touches this stage. "Arrow C++ should never be in the native
library" (2026-11-12) is honoured. D9S-2 close (2) says marks are lifted only by a green run, and the
marks here keep "Windows, by hand; Linux and CI unrun", so they comply. I also read the locked
decisions D-D9S-1..8, d9 L8/L12/L13, Q4, §8 and Findings (c), BC-11, ladder rung 3 and guard (c).

**Pressure-tested; these conform.**
- DEBT-1: brief:38, design §Windows and d9 L8 "Tests" no longer cite L12 against a link map.
- DEBT-2, Windows: the step (`ci.c-abi.yml:96-105`) comes after both creates and before "Measure"
  (`:113`).
- DEBT-2, Linux: the call (`:226`) comes after `SHIM=` (`:219`) and before the ceiling (`:252`). The
  `bash -c` block, `:204-325`, contains no apostrophe.
- DEBT-3: script `:78-81` judges with `$ARROW_PKG`, requires both `fletcher-arrow-bridge` and `arrow`,
  and fails when nothing is named.
- Shim selection: both legs pass `locate_shim.sh fletcher-c-abi variant=edge …`, and the script
  selects nothing itself.
- Wiring: no `if:` and no `continue-on-error`. The size steps are byte-unchanged (L12). Nothing under
  `c-abi/` or `dotnet/` changed, and neither did the recipe or the CMake.
- What survives: Files-to-delete is "None" and nothing survived. The only `find` is the accepted
  control residue.
- Refusals: they exit and none became a fallback. The `nm`/`grep` `|| true` paths are closed by the
  Fletcher-name guard.
- I ran it by hand on Windows at HEAD:
  - The graph reads clean over 10 host nodes. The edge DLL reads clean and carries `@fletcher@@`.
  - Graph control: red, naming `fletcher-arrow-bridge arrow`. Symbol control: red, with 1248 names.
    Exit 0.
  - `kernel32.dll` was refused ("holds no Fletcher name", exit 1). The `core` recipe was refused
    (exit 1).

## F1 — non-blocking — a production file outside Files-to-touch
- `.github/workflows/ci.pr.yml:146` (+1) adds the script to the c-abi lane's path filter. The design's
  Files-to-touch and the "2 files" declaration do not list it.
- It is benign and arguably needed. Without it, a PR that edits only the script would not run the
  lane, and guard (c) relies on that lane. It follows D9S-2's precedent for `check_shim_exports.sh`.
- **Fix:** none in code. The PM records it in the brief's As-landed: 3 files, +112/−0.

## F2 — non-blocking — tier 2 vs tier 3: "outside test requires" (owner)
- BC-11's structural test and ladder rung 3 say the edge host graph has no Arrow package "outside test
  requires".
- The design quotes BC-11 with that qualifier elided ("…"). Step 1 refuses any host node named
  `arrow`/`fletcher-arrow-bridge`/`fletcher-pubsub-arrow`. That includes a Conan `test_requires` in
  the shipped configuration, which L8 permits ("may appear only in test binaries"); the 0b audit
  noted this too.
- Unreachable today: Arrow is a plain `requires` under `run_tests` (`conanfile.py:95-107`), and step 1
  leaves `run_tests` off. But guard (c) freezes the script from D9S-4 on, so the reading must be
  settled now.
- **Suggested fix, for the owner to approve:** annotate BC-11 and rung 3 so "outside test requires"
  reads "with `run_tests` off (the shipped configuration); a `test_requires` on Arrow there is refused
  too". That matches today's stricter behaviour.
  - The alternative: if the literal reading is meant, step 1 exempts the nodes Conan marks `test`.
  - Not blocking: there is no false green, and no lawful configuration is refused today.

## RECORD
- RECORD: the design doc has not been updated for DEBT-2/3.
  - §CI wiring still puts the Windows step "after Compare the export tables" and the Linux call "after
    the export control".
  - Step 3 still says it "must name `fletcher-arrow-bridge`".
  - It still says "90 lines"; the script has 92.
  - The code follows DEBT-2/3.
- RECORD: in `design-debt.md`, the D9S-3 entries DEBT-2 and DEBT-3 are not marked as discharged
  (`52a0103`).
- RECORD: numbers. +112/−0 in 3 files against the declared ~+106 in 2. Non-comment lines are 68 (60
  in the script, 8 in YAML), inside the declared ~70–75.
- RECORD: d9 Q4 says "resolved" partway through its text but keeps the question-first form and the
  owner "shim round". The resolved rows Q1–Q3 use "Resolved <date> …" with owner "—".
- RECORD: two citations are contradicted by the tree (pre-existing; D9S-2's Conan RECORD):
  - d9 L8 "Holds at" cites `conanfile.py:58-83` and says `run_tests` is deleted from the package ID.
  - The design's §"shipped vs built" says the CI package "differs … only by `with_probe_shim`".
  - In fact `requirements()` is at `:82-107`, and the CI edge package's `conaninfo.txt` records
    `arrow/23.0.1` and `fletcher-arrow-bridge` under `[requires]`.
  - The check's conclusion stands: the graph half judges `run_tests` off, and the symbol half judges
    the built file.
- RECORD: BC-11's "incumbents" cite `conanfile.py:73-82`; it is now `:95-107` (pre-existing).
