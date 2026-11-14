# D9S-3 — step 0b test-authority audit (new-test)

**Verdict: CLEAR.** No `FORBIDDEN` or `FORBIDDEN-BY-TOLERANCE` test. No scheduled restoration. No tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md`, 419 lines, 31 entries, last heading "2026-11-13 — D9S-2 close: ceiling overrun accepted; CI evidence via the owner's draft PR", read top to bottom. d9 §3 L1–L18 read (L8, L12, L13 in full), and the design, brief, design review, locked decisions, ladder rung 3 and register BC-11.
**Scope:** `scripts/check_edge_arrow_free.sh` (uncommitted, 92 lines), and nothing else. That covers the forcing check (steps 1–2), its two controls (steps 3–4) and its refusals (no root, no Fletcher name, no file, no `nm`, no Python, conan failure).
**Yardstick:** L8 ("The edge variant links no Arrow C++ … Arrow C++ and `arrow-bridge` may appear only in test binaries"), L12 ("no link (L8), no size check, no behaviour"), L13, and the owner's ruling of 2026-11-12, "Arrow C++ should never be in the native library".

| Test | Class | Why |
|---|---|---|
| Step 1, graph half (`-o &:variant=edge`, shipped defaults) | LAWFUL | Forbids only L8's subject. `run_tests` is off, so the Arrow in test binaries is not judged. The server variant is never resolved, so D9S-4's lawful Arrow link stays green (premise a). |
| Step 2, symbol half (`nm -C` / `@arrow@@`) | LAWFUL | Judges the built artefact only. The patterns cannot match `nanoarrow::` or `@nanoarrow@@`, and the tree has no `fletcher::…arrow` namespace that could false-hit on Linux. |
| Step 3, graph control (`run_tests=True`, judged by `$ARROW_PKG`, needs both names) | LAWFUL | It asserts that Arrow appears where L8 permits it (test configuration), never in the edge build. DEBT-3 is applied (line 78). It fails if Arrow leaves the tests or the parser goes blind. |
| Step 4, symbol control (cached `c_abi_tests`) | LAWFUL | It asserts that a test binary, which L8 permits to link Arrow, shows an Arrow name. It fails on no binary, on an Arrow-free binary, or on mangled `nm` output. |
| Refusals (root guard, Fletcher-name guard, missing file/`nm`/Python) | LAWFUL | Each refusal exits non-zero. A missing cache dir also exits non-zero (errexit fires on `bin=$(find …)`). |

**Green having read nothing:** none found. An empty or garbled graph passes step 1 but fails step 3, which uses the same parser. A stripped, non-ELF or foreign file is refused by the Fletcher-name guard. Residue: the guard accepts *any* Fletcher binary, so the right file depends on the phase-B wiring passing `locate_shim.sh … variant=edge`, as the design specifies. Audit that at step 4.
**Law (b):** met. The check reads only the resolved graph and the built artefacts, and names no recipe or CMake site.
**Non-blocking notes (not violations):**
1. Step 1 does not tell a host-context `test_requires` apart, and BC-11 (tier 2) says "outside test requires". So an unconditional `test_requires("arrow")`, which L8 would allow, would be refused. Today's recipe gates Arrow under `run_tests`, so no lawful configuration is refused now.
2. The Windows symbol half's limit (an Arrow link adding no RTTI or import) is disclosed debt. It accepts nothing L8 forbids that the graph half and the Linux leg do not catch.

**Laws without a test:** this stage gives L8 its first test. L17 and L14/L15/L18 stay `[designed-not-built]`, owned by later rungs. No false green.
