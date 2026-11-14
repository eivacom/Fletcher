# D9S-7 compliance review (step 4a, cycle 1): `c3d9670..253801a` (implementation `f1a099e`, fix 1 `157d3c3`)

**Verdict: PASS.** No blocking conformance item and no STOP-AND-ASK. Read only; nothing was built.
**Proof of read:** I read `plans/RIX-rulings.md` top to bottom: 524 lines. The highest entry is at line 516, "2026-11-14 — D9S-7: CI
proves C# against the server build through a renamed copy". No tier-1 collision, and no implicit ledger contradiction touches this item.
**Yardstick:** the design, the brief, D-D9S-6 and D-D9S-8, d9 L5/L12/L13/L16/L17/§6/Q5a, BC-15, ladder rungs 9 and 10, audits 0a and 0b,
design review DEBT-1 to DEBT-4, and code review S1/S2.

## Pressure tests
1. **The staging stays within :516.** The copy lives in two `mktemp -d` folders inside the lane step (yml :120-127 Linux,
   :219-235 Windows). There is no upload and no `dotnet pack`. The server `conan create` writes only to the throwaway CI cache.
   `git diff c3d9670..253801a -- dotnet integration-tests c-abi/src c-abi/include c-abi/CMakeLists.txt c-abi/conanfile.py scripts`
   is empty: no `dotnet/` or csproj edit, D-D9S-6's one line untouched, D-D9S-8 respected. d9 §7 Q5a is unchanged. §6 only says the
   staging retires once domain 10 settles Q5a. The workflow header and ladder rung 10 credit :516 for the staging only. The edge-corpus
   choice is the design's own (annotated in the ladder, DEBT-4), not something :516 decided.
2. **[built] marks.** L17 (d9 :423-424) and BC-15 carry the N1 qualifier and "Windows by hand; CI and Linux unrun". "Unrun" is
   true: `gh run list` shows no run on this branch, so ruling :408's lift does not apply yet. There are two gaps, both listed under
   RECORD. (a) L5's new "still answer" mark (:231-233) lacks the N1 qualifier, yet its marker-equality clause and its refused-edge half
   rest on the twin alone. (b) `plans/D9S-evidence.md` has no D9S-7 row, so "Windows by hand" for rung 9 rests only on the
   implementer's report until the close writes the counts.
3. **Rung-9 mutations: record them with their provenance, do not claim them executed. Not blocking.** No tier-1 or tier-2 text
   requires them: the ladder says controls "9 and 10: green on arrival, declared" (:112), and the brief's "must turn a test red" is
   tier 3. Reading HEAD, each turns a named row red: threshold raised, `test_two_builds.cpp:143/:145/:170`; marker renamed, the
   same rows in server-first and a link failure in edge-first (`:184`); lazy check, `:156` (first copy poisoned); variant routed
   through the refusal, `:135/:175`. This matches step 2's independent trace. *Fix (PM, at close):* the D9S-7 evidence row reads "M1-M4: red as the implementer
   reported in phase A (build-folder copy, no log kept); red by reading at steps 2 and 4a; not re-run". If the owner wants
   executed proof, that takes a build run, which this review cannot do.
4. **Nothing survived that should not.** Both "first `find … -print -quit` in the cache" shim lookups are gone. The `find`s left at
   yml :107, :109 and :189 search the lane's own build folder, as the design keeps them. `f1a099e`'s always-true CONTROL copy is
   gone (S1). The dead `provider_destroy(never)` line and the design's Handled paragraph are gone (DEBT-3). No retired or
   re-anchored claim needed checking. A nit with no conformance weight: `LoadedBuild::provider_destroy` is still resolved, and
   `RequireLoaded` still requires it (`:121`), but nothing has called it since DEBT-3. Design l.28 still says "six".
5. **The edge build is unchanged, and no Arrow reaches it.** No shipped source, header, recipe or CMake changed. An edge configure
   builds nothing new: the twin block sits under `if(FLETCHER_SHIM_VARIANT STREQUAL "server")` (tests/CMakeLists.txt :288). The twin
   links impl (pubsub plus psapi/dl only, c-abi/CMakeLists.txt :67-77), pubsub and the two providers, so no Arrow. It has no
   `install()`. `package()` copies only `*{stem}` files, and the stem is `fletcher-c-abi-server` (conanfile :163-172). BC-11 and
   ruling :421 hold.

## Corner-case ladder
All seven forbidden states are still refused or unrepresentable, and none became a recovery path. The stale-edge guard is now
stronger than the design says (first RECORD line). `locate_shim.sh` exits 1 on none or several, which fails either leg.

## RECORD:
- Design l.56-58 and deviation 3 (l.121-123) still describe `f1a099e`'s control, an edge copy in a third folder. Since `157d3c3` the
  control `cmp`s the edge pass's own output (`bin/Release/net10.0/<shim>`, which must exist and differ). Rewrite both to the landed
  control. Deviation 3 then reads "closer to the brief, with no extra `dotnet test`".
- Design l.86-88, the premise that "the emitter's `conanrun.sh` covers the server shim's runtime closure": since `157d3c3` the Linux
  server pass runs under `env -u LD_LIBRARY_PATH` (DEBT-1). Restate it.
- Design Risks l.104-105 says the control "ran only by hand on Windows (11/11)". That run predates `157d3c3`, which rewrote the
  control on both legs. The close's evidence row must say either that the new control was re-run or that it is unrun.
- d9 L5 :231-233: add "edge half against a same-source test rebuild of the edge build, not the shipped edge file", and change "its
  marker text is unchanged" to "its marker text equals the other build's (A32)".
- design-debt.md, D9S-7 DEBT-2: the comment change it asks for (":285 … 'catches only the variant define'") landed in `157d3c3`
  (tests/CMakeLists.txt :285-287). Leave only the one-CMake-function half open.
- Ladder rung 9 says "about 45 landed". The D9S-7 block is 76 raw lines, 56 non-comment (tests/CMakeLists.txt :273-347).
- Brief "As landed": at HEAD, CI YAML is +81/−15 in 2 files, and the tests are 187 + 76 lines. The design's line references `:273-315`
  and `:317-344` moved down by 2 after `157d3c3`.
- `plans/D9S-evidence.md` owes a D9S-7 row: rung 9's Windows counts in both load orders, the lane at edge 11/11 and server 11/11,
  the mutations as in point 3, and a CI run ID under :408 once one exists.
