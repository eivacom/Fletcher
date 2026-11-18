# D9S-7 design review (step 2, cycle 1): `plans/D9S-7-two-builds.md`, as landed at `f1a099e`

**Verdict: APPROVE-WITH-DEBT(4).** No BLOCKER, no STOP-AND-ASK.

**Proof of read:** I read `plans/RIX-rulings.md` top to bottom: 524 lines. The highest entry is at line 516, "2026-11-14 — D9S-7:
CI proves C# against the server build through a renamed copy". The design honours :516.

**Budget.** (a) The design is 147 of its 150 lines (ruling :470); new public surface 0; net lines declared. (b) I priced the
work: CI YAML +77/−15 (net +62) in 2 files, shipped code 0, tests (~+262) not counted. That fits the 250 ceiling, so no split.
The 60-line limit that ejected the item is the light lane's entry rule, not this ceiling.

## Pressure tests
**1. Staging stays test-only (:516): yes.** The copy lives in `mktemp -d` folders inside the lane, and nothing is packed. No
csproj or `dotnet/` edit: grep finds no D9S-7 or server reference under `dotnet/` or the conformance project, and the csproj's
`Link` was already there (audit 0a). The workflow header repeats :516 ("decides nothing about how packing names files"), so Q5a
is not decided early. Check 6 does not apply: domain 10 deletes the bridge, a round D-D9S-8 puts out of reach. But only this
design records that retirement (DEBT-4).

**2. The twin is a lawful stand-in** (audit 0b law (b); the probe shim is the precedent). It matches `c-abi/CMakeLists.txt:91-208`:
four sources, define, visibility, `--exclude-libs`, export map. Drift is bounded for L5 and L17: the marker, scan, latch and
refusal all come from the one `fletcher-c-abi-impl` object library (hidden, compiled once), and the entry points from the
shared `binding.cpp`. A missing source fails the link loudly. Drift cannot quietly flip a rung-9 row; it only makes the twin a
less faithful copy of the shipped edge file, and the L17/BC-15 marks already say so (N1, verified). The copy is still kept by
hand with no machine check (DEBT-2).

**3. No deviation is a BLOCKER.** (1) Size: an entry-rule matter, and ejection was right. (2) Tests do not count, and the count
is left to `git diff --numstat`. (3) The control is equivalent, which answers code-review S1: in a fresh `-o` folder a file
under the edge name can come only from the staged path (Interop's shim items are pack-only; no `CopyToOutputDirectory` under
`dotnet/src`). So "staging the edge file turns `cmp` red" reduces to "edge ≠ server", which the control asserts on every run.
(4) 15 duplicated test lines, no hazard. (5) Stronger than the brief.

**4. The unrun Linux `bash -c` leg is acceptable, not a BLOCKER.** Every failure I traced is loud. The `&&` chain is the last
command `bash -c` runs, so any break exits non-zero; `locate_shim.sh` exits 1 on none or several; `! cmp` on a missing file is
red; `RequireLoaded` fails a failed `dlopen`. Interposition (N2) cannot fake a green: the latch is hidden in impl, every
asserted answer is resolved through the handle, and a wrongly bound variant query answers 1 where 2 is asserted. Ruling :408
sets the evidence path: "CI and Linux unrun" stays until the PR run, and the close records its run ID. The one silent route I
found is DEBT-1, unrelated to the leg being unrun.

**Mutations:** none run. By reading, each turns a row red: threshold (`FL_OK` fails :143/:145), marker rename (same, or link),
lazy check (:157), variant via the refusal (:135/:176). Compliance may record this as the waiver the design asks for, or run them.

## DEBT (registered in `design-debt.md` under D9S-7)
- **DEBT-1 (step 3, CI): the server pass checks which file is beside the tests, not which one C# loaded.** On Linux,
  `. "$RUNENV"` leaves the edge package's folder on `LD_LIBRARY_PATH`. The pass is honest only because .NET looks in the app
  and assembly folders before the OS search path: true, so unreachable, but unstated. Fix: run that `dotnet test` under
  `env -u LD_LIBRARY_PATH`. The shim's dependencies are static (`c-abi/conanfile.py:51-57`); a load failure then hits the
  design's own "fix the environment" stop.
- **DEBT-2 (round close or a later d9 stage): a fourth hand-copied shim recipe** (code-review S2). Fix: one CMake function
  builds every shim (shipped, probe, hand-over, twin) from its variant. Until then, `tests/CMakeLists.txt:285`'s "catches
  recipe drift" should read "catches only the variant define".
- **DEBT-3 (step 3, tests): the "Handled" case never runs.** On a wrong `FL_OK`, `refused.message` is null, so the `ASSERT_NE`
  at `test_two_builds.cpp:145` returns before `:148`. Fix: delete `:148` and the design's "Handled" paragraph (the row is red
  anyway; the brief said "Handled: none").
- **DEBT-4 (PM, docs): only this design says who deletes the bridge.** Add to d9 §6's domain-10 list "the conformance lane's
  server staging rename retires with Q5a". Ladder rung 10's "(ruling 2026-11-14)" should credit only the staging to :516; the
  edge corpus is the design's own lawful L13 choice.
