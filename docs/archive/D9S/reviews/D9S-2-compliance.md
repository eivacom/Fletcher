# D9S-2 — step 4a conformance review (cycle 1)

**Verdict: ISSUES(3, 1 blocking).** The code conforms to `plans/D9S-2-server-build.md` and to every
ruling. The blocking item is a close-gate evidence gap and needs no code change, so it opens no fix cycle.
**Diff:** `fd6dcd2..0a43e99` (17 files). **Proof of read:** `plans/RIX-rulings.md` read top to bottom:
**405 lines**, last heading "2026-11-13 — D9S-2: the server build is its own file" (line 395). It matches
the brief. No tier-1 collision: Decision 1(a) agrees with L5, L17, D22 and D29, and no ruling
contradicts another. Also read: design, brief, design review and DEBT, locked decisions, ladder
rung 2, 0a and 0b audits, d9 L7, L12, L13, L16, and register BC-10 and BC-11.

## Checked and conformant (with evidence)
- **Switch:** `edge`/`server`, default edge, kept in the package ID (only `run_tests` is deleted). `generate`, `package` and
  `package_info` follow it, and the edge copy patterns are byte-identical. `validate()` refuses server with the probe (`conan graph
  info`: `info_invalid`). Conan refuses an unknown value, and CMake `FATAL_ERROR` (`:28`) comes before `add_subdirectory(tests)`.
- **BC-10, one site:** `binding_version.c` with `#ifndef`/`#error`, compiled only into the shipped target and the probe (probe = edge).
  No `#if` in `c-abi/src`. The recipe's stem names the file, not the answer.
- **No Arrow** in either build. **No edit under `dotnet/`.**
- **What survived that should not:** none of the six c-abi/dotnet `find -print -quit` lookups, `shim-exports.txt`, the lone
  version grep, "exports one function" or the edge literal. The X-1 integration-lane lookups stay, as designed (P3 holds; DEBT-1).
- **Tests:** BA-V is re-anchored per artefact (`#error`, `static_assert`; wire literals kept). `example.c` takes its expected
  build from the package. The server create runs all of `c_abi_tests`.
- **CI wiring as designed:** the server create runs first; the edge create passes no `variant` (guard); the edge message is
  scoped; the server size is printed (DEBT-4); the export step and the control run on both legs; sparse checkout and filters are in place.
- **Run here (Windows, local cache):** `locate_shim.sh` picks exactly one file for edge+probe, server and probe. It refuses
  `variant=edge` alone as ambiguous, listing both, and gives a named error for a misspelt option, the wrong variant or no
  options. `check_shim_exports.sh`: edge = server = 48 names; the probe in the server's place is red, naming both `fl_test_*`.
  Named errors for no dumpbin, an empty header and an unknown file type.
- **Comment trimming:** I judged the final text (no draft survives). Every comment the design requires is present (the
  `binding_version.c` site/D9S-4 note, the scoped error text, the corrected stale note, "passes no `variant` on purpose",
  and the citations of laws and rulings). Only lines the design orders deleted were removed. Nothing required was lost.

## F1 — BLOCKING (close gate; no code change): the marks claim more than has run
Half of rung 2's forcing tests are CI by design (mapping rows 4–5: "both legs", "c-abi and dotnet
lanes"). Neither `ci.c-abi.yml` nor `ci.dotnet.yml` has run on `0a43e99`, and no Linux body has
run at all: the `nm` branch, the `.so` locate, the Linux server compile and suite, and the Linux
control. Yet the docs already say: d9 L7 "compares … on both platforms"; L7 Tests "both legs";
L12, L13 and L16 `[built]`; §2 "Server(-tier) variant `[built]`"; register BC-10 "export-set check
`[built]`"; BC-11 "selection by package identity `[built]`". The register's rule is "a build-state
mark scoped to what is actually built".
**Fix:** before close, get one green run of `ci.c-abi.yml` and `ci.dotnet.yml`, both legs, on this
branch, and record the run IDs in `D9S-evidence.md`. That run must show the server create, the
edge locate, both export comparisons and the red-by-name control. If a run is not possible, narrow
the marks to "Windows, by hand; CI unrun at `0a43e99`".

## F2 — non-blocking: a dumpbin failure goes red without saying why
`check_shim_exports.sh:52`: dumpbin writes its diagnostic to **stdout**, and `table=$(table_of …)`
swallows it. I gave it an empty `.dll` and a garbage `.dll`: it exits 112 or 83 and prints nothing
after the edge line. It still fails, so this is not a vacuous pass, but it does not "fail loudly
naming the cause" (corner case 7's spirit). On Linux, `nm` errors go to stderr and stay visible.
**Fix (about 3 lines):** keep dumpbin's output. On a non-zero status, print
`::error::dumpbin could not read <shim>` followed by that output. Optionally also refuse
`EDGE == SERVER` (the same file passed twice compares the server set vacuously). CI cannot do this
today, because the two paths have different stems.

## F3 — non-blocking: the one-off controls the design orders are not yet in evidence
Mapping rows 4–5 each require one control run "once, in evidence". (a) A throwaway `fl_` function
compiled into the server target only must be red by name: there is no trace of this run. (b) Two
edge packages in one cache: `variant=edge` alone refused, and `with_probe_shim=True` picks the probed
one. I reproduced (b) on the local cache; it holds on Windows.
**Fix:** record (a), and (b) with its output, in `D9S-evidence.md` before close.

## RECORD
- RECORD: d9:309 still says "The server-tier variant … none of it is built yet", which contradicts the new L12, L13 and L16 `[built]` marks.
- RECORD: `c-abi/README.md:127-128` still says "Both the shipped shim and the test-only probe shim answer edge", but the shipped shim in a server package answers server.
- RECORD: some line citations have drifted. d9 §2 Variant and L12 Precedent cite `CMakeLists.txt:164-211`; the probe block is now `:200-235`. The L7 "Holds at" CMakeLists/conanfile lines have moved. BC-11 cites `conanfile.py:73-82` for the `run_tests` Arrow block, which is now at about `:94-106`. The d9 §2 "Shim" row names only `fletcher-c-abi`.
- RECORD: `ci.dotnet.yml:119` comment still reads "The find is checked rather than trusted"; the lookup is now `locate_shim.sh`.
- RECORD: production is +313/−64 (net +249), against the declared +197/−68 (net +129) and DEBT-3's realistic ~+185. That is under the 250 ceiling but about +93% net over the declared figure. Book it at close.
- RECORD: the move of n1 to D9S-7 (design review point 5) and the `*Ingested*` note on the 2026-11-13 Decision 1 ruling are still owed at close.

## Cycle 2

**Verdict: ISSUES(2, 2 blocking).** C2-1 is a code item that the fix cycle introduced. The other is the rest of F1,
which is docs only: the PM narrows it in place and it opens no fix cycle. Diff `0a43e99..e676e1c` (6 files).
Ledger re-read: 405 lines, same last heading (line 395), no new ruling.

**F2 — CLOSED.** I re-ran it. An empty `.dll` gives `::error::dumpbin could not read …` with LNK1136 and
exit 1; a garbage `.dll` does the same with LNK1107. Edge = server = header still passes, and the
control still names `fl_test_probe_loan`. **F3 — CLOSED as to the run** (the coordinator reports it;
I did not reproduce it). No `.md` mentions `fl_zz_throwaway`, so its output is still owed in evidence (RECORD).

**F1 — OPEN (rest only; docs; blocking for close).** L7's mark line, d9:309, L12 and L13 are now
narrowed. Six places still say built with no qualifier:
- d9:85 §2 "Server(-tier) variant": `[built]` as an artefact.
- d9:86 §2 "Variant query" and d9:371 L16: `[built]` for all three artefacts.
- d9:528 Q4: "CI by package identity built". That selection lives only in YAML that has never run.
- Register BC-10: export-set check `[built]`. BC-11: "selection by package identity `[built]`", likewise all unrun YAML.
**Fix:** add the same qualifier ("Windows, by hand; CI and Linux unrun") to each. d9:265 (L7 Tests) can stay as it is: it sits under a qualified mark.

## C2-1 — BLOCKING: the server file name is no longer tied to the build at the CMake door
The 4b S1 fix replaced the design's `OUTPUT_NAME fletcher-c-abi-server` under server (design "The
artefact"; ruling 2026-11-13 "the server build's native file has its own name") with a free
`FLETCHER_SHIM_OUTPUT_NAME` cache string. I reproduced the logic in a scratch CMake project:
(1) `-DFLETCHER_SHIM_VARIANT=server -DFLETCHER_SHIM_OUTPUT_NAME=fletcher-c-abi` configures a server
build that carries the edge build's file name. (2) A build folder first configured as server, then
reconfigured with `-DFLETCHER_SHIM_VARIANT=edge`, keeps the cached `fletcher-c-abi-server`: an edge
build under the server's name, silently (L12: the edge file name does not change). (3) README:91 ("a direct
configure takes `-DFLETCHER_SHIM_VARIANT=edge|server`") is now false for server. Conan builds are safe,
because `generate()` writes both values from one option.
**Fix (either one):** (a) the recipe stays the only decider, and CMake refuses any name that is not the variant's own
(server: `fletcher-c-abi-server`; edge: empty or `fletcher-c-abi`). Or (b) restore the design's shape
(CMake sets `OUTPUT_NAME` from the variant). The new `package()` empty-copy raise, which stays in
both cases, then makes drift loud, which is the hazard S1 named. Correct README:91 either way.

## RECORD (cycle 2)
- RECORD: README:127-128 and `ci.dotnet.yml:119` are fixed. README carries no build marks, so it had nothing to narrow.
- RECORD: cumulative production `fd6dcd2..e676e1c` is +345/−74, **net +271, over the 250 ceiling**; book it at close.
- RECORD: the `fl_zz_throwaway` output goes in `D9S-evidence.md`. The cycle-1 citation drift and the n1/Ingested notes are still open.

## Cycle 3

**Verdict: PASS (conforms).** Diff `e676e1c..849b12d` (5 files). Ledger unchanged: 405 lines, last heading at line 395.
**C2-1 — CLOSED.** The design's shape is back: under server, CMake sets `OUTPUT_NAME fletcher-c-abi-server`; under edge it sets
nothing. `OUTPUT_NAME` is a target property, not a cache entry, so a reconfigure cannot carry a stale name. No
`FLETCHER_SHIM_OUTPUT_NAME` is left anywhere. The `package()` empty-copy raise is kept, and README:91 is true. The implementer's log
(scratchpad `d9s2_fix2.log`) shows a direct server configure giving `fletcher-c-abi-server.dll`, the same folder reconfigured
to edge giving `fletcher-c-abi.dll`, a forced stem drift raising `no shim named 'fletcher-c-abi-zzdrift'`, and 99/99 and 110/110 passing.
**F1 — CLOSED.** d9:85, :86, :371 (L16), Q4 and register BC-10/BC-11 are now qualified ("Windows, by hand; CI and Linux unrun").
`grep D9S-2` finds no unqualified build claim left. d9:265 (L7 Tests) describes wiring under a qualified mark.
- RECORD: F3's output is only in the session scratchpad (`d9s2_fix1.log:1428-1433`, red naming `fl_zz_throwaway`). Copy it into `D9S-evidence.md` at close.
- RECORD: cumulative production `fd6dcd2..849b12d` is +339/−74, **net +265, over the 250 ceiling**. CI evidence is owed at the PR. The cycle-1 citation drift and the n1/Ingested notes are still open.
