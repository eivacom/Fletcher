# D9S-7 — step 0b test-authority audit (new-test, light lane) — HEAD c3d9670 + working tree

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test. No tier-1 disagreement. The design needs no correction.
**Proof of read:** `plans/RIX-rulings.md` read in full: 524 lines. Highest entry, line 516: "2026-11-14 — D9S-7: CI proves C# against the server build through a renamed copy".
**Yardstick:** d9 L5, L12, L13, L16, L17; register BC-11, BC-15; rulings 2026-11-12 "one shim, two variants", 2026-11-13 "Packaging", "Wrong build", "the server build is its own file", "a test-only Arrow dependency of the edge build is refused too".
**Scope:** `c-abi/tests/test_two_builds.cpp:127` `TwoBuilds.TheLaterBuildRefusesNamingBothAndTheFirstKeepsWorking`, `:162` `TwoBuilds.ARefusedBuildStillAnswersItsVariantAndVersion`, its helpers `OtherPath`/`Other`/`RequireLoaded`; executables `c_abi_two_builds_server_first` and `_edge_first`; the twin `fletcher-c-abi-edge-twin` (`c-abi/tests/CMakeLists.txt:272-345`). All LAWFUL.

- **Law (a), forbidden or tolerated behaviour:** none. Test 1 asserts L5 as written ("refuses every fallible call, naming both modules"; "Only the later-loaded copy is poisoned"). Load failure, same-build-twice and mis-staging all fail the test instead of passing vacuously (`RequireLoaded`, the two variant assertions). The `provider_destroy` at `:148` only cleans up after a failed EXPECT. It does not accept a second working shim.
- **Law (b), the twin:** it is a lawful external proof of L5/L17's mechanism. The twin is the edge target's source list, defines, visibility, export map and links, copied into the test CMake (checked against `c-abi/CMakeLists.txt:91-210`). The marker comes from the shared `fletcher-c-abi-impl`. This follows the probe-shim precedent (L12). The half it leaves unproven, as the brief says: the shipped edge file itself, and drift between the copied recipe and the real one. Test 1's variant assertion catches only the variant define. **Non-blocking finding N1:** when d9 L17 and BC-15 become `[built]`, the mark must say "against a same-source test rebuild of the edge build, not the shipped edge file", plus the usual Windows/CI/Linux marks. A bare `[built]` would overstate what was proven.
- **Green on arrival:** this is a lawful guard rung. Ladder rung 9 declares it "Expected green on arrival; declared". All four mutations hit code paths the laws name:
  - scan threshold: L5 refusal;
  - marker rename: L17. In one load order the red is a link failure, not an assertion. That is acceptable;
  - lazy check: "only the later copy is poisoned";
  - variant routed through the refusal: L16.
  It is not a forcing test that proves nothing. It closes the 0a gap (no real-second-shim test of L5 or L17).
- **New value subjects:** two, both moored.
  - "A refused copy still answers its variant" rests on L16 ("callable before anything else … as `fl_binding_abi_version` is") and L12 ("answers truthfully"). L5's "still answer" bullet needs to list the query too. The brief already counts that as a doc owed.
  - "The marker text equals the linked copy's" rests on L13 ("the only entry points whose answers differ are the variant query and the batched ones"; A32).
  Nothing UNMOORED.
- **Can the twin ship, or put Arrow on the edge?** No.
  - It is built only in tests, and only when variant=server. There is no `install()`.
  - `package()` copies `*{stem}.dll`, `*lib{stem}.so*` and `*{stem}.lib`, where stem is `fletcher-c-abi-server`. `fletcher-c-abi-edge-twin` matches none of them. No CI glob names it.
  - It links impl + pubsub + the two providers, and no Arrow. Arrow enters only `fletcher-c-abi` under server.
  - The edge package's graph is untouched, so BC-11 and the 2026-11-13 test-only ruling hold.
- **Non-blocking N2 (disclosed in the brief):** on Linux, in edge-first, the refused server copy's calls to its own exports can bind to the edge twin. The test resolves through the handle, and the refusal comes before any body runs, so no assertion depends on this. It is a production-code observation, outside this audit's yardstick.
- **Scheduled restoration:** none.
