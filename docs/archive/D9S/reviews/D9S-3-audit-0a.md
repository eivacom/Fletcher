# D9S-3 — test-authority audit, step 0a (corpus)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test in scope. No scheduled restoration. No tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md` has 419 lines (`wc -l`; the brief expected 418). The last heading is "2026-11-13 — D9S-2 close: ceiling overrun accepted; CI evidence via the owner's draft PR", as expected. I read d9 §3 L1–L18, focusing on L8 and L12, and register BC-11.

**Yardstick:** L8 "The edge variant links no Arrow C++ … Arrow C++ and `arrow-bridge` may appear only in test binaries". L12 "no link (L8), no size check" changes on the edge. Ruling 2026-11-12: the edge variant is "never carrying Arrow C++ (it must run on embedded devices)". Ruling 2026-11-13 (1): "the server variant ships in its own .NET package".

**Scope covered:**
- `ci.c-abi.yml`: the edge size ceilings (win 12 MiB, linux 20 MiB), the server size printed only, and the export checks with the probe control.
- `ci.dotnet.yml`: shim selection, strip check and dependency check.
- `scripts/check_shim_dependencies.sh`, `check_shim_exports.sh`, `locate_shim.sh`, `check_dotnet_packages.sh`, `check_conan_lockfile.sh`, `check_third_party_notice.sh`.
- `c-abi/conanfile.py` (Arrow and `arrow-bridge` only under `run_tests`; `variant` kept in the package ID).
- `c-abi/tests/CMakeLists.txt` and `*.cpp`/`*.c`: no DISABLED_ or GTEST_SKIP.
- `integration-tests/binding-abi-conformance` and its workflow.

**Results:**
- **LAWFUL:** the edge ceilings (a lawful proxy for L8). T10: the test binaries link Arrow, which L8 permits. T14: the conformance emitter links Arrow and the shim does not.
- **LEGITIMATE-BUT-ADJACENT, RE-ANCHOR (not delete), not D9S-3's:**
  - What it checks: `scripts/check_dotnet_packages.sh:113`, "carries native assets, which only Eiva.Fletcher.Interop may (D-BIND-13)".
  - The conflict: ruling 2026-11-13 (1) revised D-BIND-13 ("two packages, each a full shim").
  - Why re-anchor and not delete: the subject (native assets only in the sanctioned packages) is lawful; only the list is stale. It cannot fire today because no server package exists.
  - Where it is tracked: T6. Deferred to domain 10's packaging round (ladder Finding 4).

**Laws with no test:**
- L8 is `[built as a property; no code owner]` and has no test asserting it by name. That is this stage's subject (rung 3; BC-11 `[designed-not-built]`).
- No false green: no test claims BC-11 as built.
