# D9S-2 — design review (cycle 1)

**Verdict: APPROVE-WITH-DEBT(4).** No BLOCKER, no STOP-AND-ASK. Design `plans/D9S-2-server-build.md`,
brief `plans/D9S-2-brief.md`, tree `cf22af8` (+ `6108b90`).
**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **393 lines**, last heading "2026-11-13 — D9S
pre-round: a failed handoff is reported; corrections approved" (line 383). Matches the brief. Also read:
d9 §1, §3 L1–L18, §4–§7; register BC-10, BC-11, BC-15; `D9S-locked-decisions.md`; the ladder; the
pre-round audit (T3–T7, T19, C1, C2, C10, F7, F8, F13, F14); the 0a audit; D9S-1's brief and code review.

## Tree claims verified
- `binding_version.c` is compiled into the shipped target (`CMakeLists.txt:81`) and the probe (`:182`)
  separately, never into the object library. So a per-target definition reaches exactly one artefact.
- All 48 `^FL_ABI_EXPORT` lines in `binding.h` have the name before the first `(`. That includes `:916`,
  which breaks the line after the `(`. The `#define` lines do not start with the macro.
- The probe's extra exports `fl_test_probe_loan` and `fl_test_probe_score` (`probe_exports.cpp:49-78`) are
  inside the version script's `fl_*`. So the control can only go red through set equality, which is
  the thing it is meant to prove. `exports.map` is an anonymous node (`:46-51`), so `nm -D` prints bare names.
- `PoisonedForThisTest` poisons the test executable's own copy of the internals
  (`test_containment.cpp:24-27, 72-77`).
- The lookups and the T4 block are at the lines the design cites (`ci.c-abi.yml:91-96, 157-162, 231-247`;
  `ci.dotnet.yml:137-148, 285-298`). `SingleCopy.*` match the substring `fletcher-c-abi`
  (`test_single_copy.cpp:95, 111`).
- No consumer recipe sets any c-abi option other than `with_probe_shim` (P3).

## The six points

**(1) The `variant` option vs L12 and BC-10/BC-11: lawful.** There is one recipe, one CMakeLists and one
source list. The builds differ by one compile definition and `OUTPUT_NAME` (L12, "same sources"). BC-10's
single site is still `binding_version.c`, fed by one per-target definition. The `#error` is in that site,
so no second `#if` exists. BC-11's site reads "the recipe's variant switch, kept in the package ID"
(F13), and this option is that switch. The file stem is derived twice (CMake `OUTPUT_NAME`, recipe
`package()`/`libs`), as the probe's is today. If the two disagree, the package is empty and
`test_package` fails to link, so the failure is loud. This shape also makes rung 3's "edge configuration"
a single `conan graph info -o variant=edge`.

**(2) No Arrow at rung 2: lawful sequencing, nothing to undo.** The ladder says "No Arrow link yet" and
puts rung 3 before rung 4. D9S-4 adds a `requires` and a link under `variant == "server"` beside what
exists. `validate()`'s refusal of server plus probe blocks neither D9S-6's fault seam (a test artefact
built under `run_tests`, not the probe) nor D9S-7's test-only edge artefact.

**(3) `locate_shim.sh` is the single site for the lanes that check the edge shim** (T3, T4, T5, T19).
Every lookup there goes through it. It fails on zero matches and on more than one, naming what it found.
A misspelt option or a false P1 gives zero matches, so it fails loudly too. It is **not** the single site
tree-wide: the six X-1 lookups (`ci.integration-test.{binding-abi-conformance,gateway-dotnet-crosslang,
pubsub-transport-conformance}.yml`) still go by file name. That is lawful (0a X-1) and held by corner
case 9 and P3. Those lanes check nothing about Arrow, so BC-11's "by package ID" is not breached. P3 will
fire at D9S-7, when rung 10 creates a server package in the conformance lane (DEBT-1).

**(4) The export check proves what L7 and the ladder ask.** On Linux it checks the whole dynamic table
(only `fl_*` plus the linker's five names) and that each build's `fl_*` set equals the header's. On
Windows it checks that each build's `fl_` subset equals the header's. That is the declared D-BIND-157
residue. The probe control goes red by name, through equality, on every run on both platforms, so the
comparison is shown to fire rather than assumed to. Two guards close the vacuous pass: both version entry
points must be in the declared set, and the export list must be non-empty. One thing to watch at close:
L7 may become `[built]` for `fl_` equality on both platforms, but the whole-table clause stays "violated
on Windows (accepted)".

**(5) Moving n1 to D9S-7 is sound.** The only poisoning in the suite is of the test executable's own
copy, so a row built on it would pass whether or not the shim answers when refused. Rung 9's subject
already includes "in the refused one, the version and the variant query still answer". D9S-1's close
booked n1 into D9S-2, so the tracker should record the move and its reason.

**(6) Brief Decision 1 is well-formed.** It has two options, each with a users line and an architecture
line naming a law (L17, D22), a recommendation grounded in the purpose and L17, and a default. No ruling
answers it. L5 gives "how the two files coexist on disk" to the shim and C# rounds' design. Q5a gives the
native name to this round, and ladder Finding 5 says either option is lawful. So it is the architect's
call, put to the owner as a confirmation with a default, which is acceptable. Two accuracy fixes are owed
before the owner reads it (DEBT-2).

**The workflows are in scope.** `ci.c-abi.yml` is this domain's lane. The `ci.dotnet.yml` edits change
only how the edge shim is found (T5, T19), which ladder rung 2 and the 0a audit assign here. They touch
nothing under `dotnet/` and no NuGet packaging (D-D9S-8). The `ci.pr.yml` filter lines follow that file's
same-commit rule (`ci.dotnet.yml:60-61`). None of these is domain 10's code.

## Check 7b: the implied work against 250 net production lines
I priced the work myself rather than taking the design's number. It commissions:
- 2 new scripts
- about 4 recipe methods
- 1 cache variable with its check, 2 definitions and `OUTPUT_NAME`
- 1 C line with its guard
- 2 creates and 6 lookups
- an export step and a control on each leg
- 2 sparse-checkout entries and 3 filter lines

At this tree's density (`check_shim_dependencies.sh` is 92 lines for comparable parse-and-refuse work)
I expect about +250/−68, net about +180 to +190. **It fits, so no split is needed.** The declared +197/−68
is low (DEBT-3). Document budgets: design 259/300, brief 53/60, new public surface 2/3.

## DEBT (registered in `plans/reviews/design-debt.md` under D9S-2)
- **DEBT-1 (hand-off to D9S-7).** When rung 10 creates a server package in the conformance lane, P3 fires.
  Re-anchor `ci.integration-test.binding-abi-conformance.yml:86, :150` with `locate_shim.sh` in that
  commit. Also annotate the ladder's rung 9 "Builds on": both artefacts are no longer in one test tree,
  so rung 9 now costs R1's 10–20 lines.
- **DEBT-2 (PM, before the owner reads the brief).** (b)'s users line presents "whichever is copied last
  silently wins" as a consequence of the native name. But the NuGet-level name is domain 10's, and a
  `.csproj` can pack the file under any name (design, "Q5a, shim half"). Make the line conditional:
  "unless the C# switch round renames the file when packing". (a)'s architecture line should also cite
  D29: an app that references the server package loads the server build. That is the ruling (b) puts at
  risk.
- **DEBT-3 (close).** Book the realistic production figure (about +250/−68, net about +185, under the
  ceiling) so the close does not read as an overrun.
- **DEBT-4 (step 3, or D9S-4 at the latest).** The pre-round audit's T3 row says: "The server shim gets
  its own measurement and no Arrow-named ceiling unless ruled (R-9)." The design prints nothing for the
  server shim. Print its size on both legs and never enforce it (about 3 lines per leg), so domain 10's
  R-9/Q5a ceiling question has a measured number.
