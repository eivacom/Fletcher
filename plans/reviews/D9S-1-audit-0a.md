# D9S-1 — step 0a test-authority audit (MODE=corpus) — verdict: **CLEAR**

**Proof-of-read:** `plans/RIX-rulings.md` read in full: 393 lines, last heading "2026-11-13 — D9S pre-round: a failed handoff is reported; corrections approved". d9 §3 (L1–L18) and §5 (D1–D31) read; audit part C (T1–T19), ladder rung 1, D-D9S-6 read.
**Yardstick (tier 1):** d9 L6 (version reported; every change bumps; one version for both builds, D27), L7 (export table = surface), L12 (edge gains only the query and refusals), L13, L16 (variant query, both builds), L5/L17; rulings 2026-11-13 "Wrong build", "Version: One version; edit the line", "C# loads: Depends on package".

**Scope covered (incl. controls and skips; no `DISABLED_`, `GTEST_SKIP`, `WILL_FAIL` or xUnit `Skip` found):**
- `c-abi/tests/test_binding_abi.cpp:17,24` `BindingAbi.VersionMatchesHeader`, `.VersionIsPackedMajorMinor`: LAWFUL (L6). They compare against the header constant, so the bump does not break them.
- `c-abi/tests/c99_probe.c` + ctest build row (`tests/CMakeLists.txt:111-126`): LAWFUL (L7).
- `test_binding_entry_points.cpp:834` `TheShippedShimHasNoProbe`, and the 54 entry-point cases: LAWFUL. None pins an export count or the set of entry points, and none says a build must lack the query.
- `test_probe_shim.cpp` (7) and `test_single_copy.cpp` (5): LAWFUL (L5/L17, D7). None asserts that the probe shim is a different build from edge, so rung 1's "probe answers edge" contradicts nothing.
- `test_containment.cpp` (6), `c-abi/test_package/src/example.c`: LAWFUL (L1, L5, L6).
- `dotnet/tests/Fletcher.Tests/NativeLoaderTests.cs:44,79,107` (handshake, accepts own, refuses any other): LAWFUL (L6). They read `NativeLoader.HeaderVersionMinor`, and the line-34 edit is the one ruled scope exception (D27, D-D9S-6). The other loader cases (marker, resolver, candidate paths, OS refusal) are LAWFUL.
- `integration-tests/binding-abi-conformance` (`src/emit_corpus.cpp`, dotnet project, CI yml): LAWFUL. It pins no version and no exports.
- `ci.c-abi.yml:231-247` Linux export check (`fl_` prefix + `fl_binding_abi_version` present); `cmake/exports.map` `fl_*`: LAWFUL (L7). A new `fl_` query passes it. `scripts/check_shim_dependencies.sh` checks imports only: LAWFUL.

**Not found:** no manifest row, debt item or comment schedules the return of any forbidden test, and no test is forbidden. No tier-1 disagreement: L6's no-stub rule and L12's refusals are reconciled in the law text (D28).
**Laws without tests (gaps, not violations):** L16 and L12 are `[designed-not-built]` with no test. That is correct, and rung 1 creates the L16 test. L6's append-only and every-change-bumps rules are `[built as a property; no test]`. L7's exact-set clause has no test: the CI check is prefix-only, though its error text says "binding.h does not declare" (F7/BC-10, rung 2).
**Stale prose, not a test:** the comment at `ci.c-abi.yml:25` says "an ABI that exports one function".
