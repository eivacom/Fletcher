# D9S-2 — step 0a test-authority audit (MODE=corpus)

**Verdict: CLEAR.** No `FORBIDDEN` or `FORBIDDEN-BY-TOLERANCE` test in scope. Nothing is deleted. There is no scheduled restoration and no tier-1 disagreement.

**Proof of read:** I read `plans/RIX-rulings.md` from top to bottom. It has **393 lines**, and its last heading is "## 2026-11-13 — D9S pre-round: a failed handoff is reported; corrections approved" (line 383). Both match the brief.
**Yardstick:** d9 §3 L1–L18, with emphasis on L7, L8, L12, L13 and L16, plus BC-10 and BC-11. Also every RIX ruling, especially 2026-11-12 "one shim, two variants" and 2026-11-13 D22–D30. The spec, the pre-round audit, the ladder and the locked decisions served as context only. Tree: `cf22af8`.

## Scope covered
- `c-abi/tests/`: 105 gtest rows in 6 files, plus `c99_probe.c` (ctest `BindingAbi.CompilesAsC99AndIsSelfContained`), `single_copy_decoy.c` and `codec_corpus.hpp`. No row is `DISABLED_`, skipped or declared red.
- `c-abi/test_package` (`example.c`); `c-abi/conanfile.py` (`run_tests`, `with_probe_shim`, `package_id`); `c-abi/CMakeLists.txt`; `c-abi/tests/CMakeLists.txt`.
- `integration-tests/binding-abi-conformance`: `emit_corpus.cpp`, `CorpusConformanceTests.cs` and the recipe.
- Workflows: `ci.c-abi.yml`, `ci.dotnet.yml`, `ci.integration-test.binding-abi-conformance.yml`. I grepped all workflows for shim lookups by file name, which added `ci.integration-test.gateway-dotnet-crosslang.yml` and `ci.integration-test.pubsub-transport-conformance.yml`.
- `scripts/check_*`: `check_shim_dependencies.sh` (T5), `check_dotnet_packages.sh` (T6) and `check_conan_lockfile.sh` / `check_third_party_notice.sh` (T8, T9, read for their dependence on the default graph).

## Flagged tests: all LEGITIMATE-BUT-ADJACENT, so RE-ANCHOR and never delete
Each subject below is lawful. Only the vehicle changes, which is how the test picks the shim or what value it expects.

| # | Where | Subject (lawful by) | Vehicle being retired | Disposition |
|---|---|---|---|---|
| T3 | `ci.c-abi.yml:91` (win), `:157` (linux): `find … fletcher-c-abi.dll` / `libfletcher-c-abi.so … -print -quit`, ceilings `:103-111`, `:188-196` | Edge shim stays small, as a proxy for "no Arrow C++" (L8 "The edge variant links no Arrow C++"; L12 "no size check" changes) | Selection by file name. Once a server package sits in the cache, `-print -quit` may measure the server shim, and the error text ("catch Arrow C++ being linked into the shim") would then refuse a lawful server build. | **RE-ANCHOR** the selection to the edge package by package identity. Keep the numbers. Scope the message to the edge build (D3 is partly superseded for the server build). |
| T4 + glibc floor | `ci.c-abi.yml:223-247`, using the same `$SHIM` | Exports are `fl_*` only (L7); glibc floor printed | Same `$SHIM` variable | **RE-ANCHOR** together with T3. EXTEND T4 as rung 2 plans (edge = server = `FL_ABI_EXPORT` set, L7/L12). |
| T5 | `ci.dotnet.yml:137`, `:285` (shim); `:143`, `:292` (probe); `check_shim_dependencies.sh` | Edge shim imports only OS/CRT, with exact floors (D-BIND-148/149) | Selection by file name | **RE-ANCHOR** to the edge package. The server shim's run waits on R-9 (domain 10). |
| T19 | `ci.dotnet.yml:161-168` strip check | Stripping leaves the export table unchanged | Rides on T5's `$SHIM` | **RE-ANCHOR** together with T5. |
| BA-V | `test_binding_abi.cpp:48` `TheVariantIsAnsweredBeforeAnyOtherCall`, `:64` `TheVariantAndVersionAnswerAlikeFromManyThreads`, which hard-code `FL_VARIANT_EDGE` | The query answers truthfully (L16) | The expected value is fixed to edge. That is correct for `c_abi_tests` and `c_abi_probe_tests`. | **RE-ANCHOR** the expected answer per artefact when the file is compiled into the server executable (rung 2's red). Never weaken it to "edge or server". |
| X-1 | `ci.integration-test.binding-abi-conformance.yml:86`, `:150`; `…gateway-dotnet-crosslang.yml:79`, `:156`; `…pubsub-transport-conformance.yml:91`, `:174` | Lanes that load "the shim" by file name | Selection by file name. Unambiguous today, because these lanes run `conan create c-abi` with default options only. | **LAWFUL as long as each lane builds only the default.** The design must keep the default as edge, or else re-anchor these lanes too. Rung 10 (stage 7) owns the conformance lane's server run. |
| X-2 | `ci.dotnet.yml:175`, `:319`: `conan install --requires=fletcher-c-abi/[*] -o with_probe_shim=True` feeding the notice (T8) and lock (T9) | Notice and lock describe the shipped graph | Resolution through default options | **LAWFUL as long as the new switch defaults to edge.** If not, T8 and T9 would silently describe the server graph. |

## Not this round's (D-D9S-8), recorded
- **T6** `check_dotnet_packages.sh:112-113` ("carries native assets, which only Eiva.Fletcher.Interop may (D-BIND-13)") and `:117-118`. The subject, "no stray native assets", is lawful. Its citation of D-BIND-13 is stale, because D22 revised D-BIND-13 to "two packages, each a full shim". **LEGITIMATE-BUT-ADJACENT, RE-ANCHOR in the round that adds the server package (domain 10)**, as ladder Findings 4 says. D9S produces no server package, so nothing exercises this line now.

## Checked and LAWFUL (no action)
- `TheShippedShimHasNoProbe` (L13: three built-ins on both builds).
- `SingleCopy.*`: the name match "fletcher-c-abi" survives a distinct server file name only if that name keeps the substring. The architect should note this for rung 9, Q5a.
- `Containment.*`, `NanoarrowCodec.*`, `DecodedSchema.*`, `Subscriber.*`, `Attachments.*`, `Blobs.*`, `TopicOptions.*`, `Publisher.*`, `ProbeFixture.*`.
- `c99_probe.c` and `test_package/example.c`, which accepts either variant and is lawful against both packages.
- `tests/CMakeLists.txt` Arrow links (T10; L8 allows test binaries).
- `package_id` keeps `with_probe_shim` and drops `run_tests`. The new switch must stay in the package ID (F13).

## Law build-states vs corpus
- No `[designed-not-built]` law is asserted as built. L12–L15, L17 and L18 have no test, which is correct.
- `[built]` laws with no test: L7's export-set clause (CI checks only the prefix; rung 2 extends it), L2's enum equality, L6's append-only rule, and L3's "OUT untouched". L8 has no test at all (marked "no code owner"; rung 3).
