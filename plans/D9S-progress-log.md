# D9S — Progress log

## Planning — 2026-11-13

Round designed by `round-designer`. Domain 9 created and specified (owner-confirmed 2026-11-13);
rulings: separate server package, edge refusal + variant query, codec to domain 2, laws confirmed;
batch borrow-and-move (B1), one version + one NativeLoader line, FL_NOT_SUPPORTED widened, C#
loads by package (revision in place), failed handoff reported (L18), corrections approved.
Audit: 115 rows (66 OK, 46 FINDING, 3 BLOCKER rows = B1, resolved). Ladder: 10 rungs, 7 stages.
**PM decision:** the ladder moves the version bump to stage 1 (D-D9S-6), one bump, satisfying D27.

## Kickoff — 2026-11-13

**Directive (owner):** "Run all seven items". D9S-1 through D9S-7, stopping only for a Stage Brief
with decisions, a stop-and-ask or a blocker; commit and push each item to
`origin feature/shim-server-variant` at close. Light lane enabled by size (D9S-1, -3, -7 candidates).
**Preflight:** all required keys present; purpose passes the strike test. Plugin source 0.10.0 vs
marketplace 0.9.1, as before; skills load from the source checkout.

## D9S-1 — A caller can ask which build the shim is — 🟢 2026-11-13 (light lane)

**Landed:** `fcb7be6`. `fl_binding_variant()` answers `FL_VARIANT_EDGE` from one site
(`binding_version.c`, BC-10), callable first and from any thread; edge and probe shims answer edge.
ABI 0.7 → 0.8 with the one permitted domain-10 edit (`NativeLoader.HeaderVersionMinor = 8`, D-D9S-6).
**Steps (light lane):** 0a CLEAR · brief `5f573ea` (no decisions) · phase A red (compile: query
absent, both test executables) · 0b CLEAR · green · 4b 0 blocking, 4 nits, conforms.
**Verification:** c-abi `conan create` run_tests + probe, 110/110 (implementer, PM-checked log);
control: C# `NativeLoaderTests` 11/11 at 8, 2 red per target at 7 (restored); full suite:
binding-abi-conformance 11/11 against the 0.8 shim, pubsub-arrow-fastdds 4/4 (PM).
**Numbers:** production +23/−2 (19 touched, 3 files) vs declared +25/−3; tests +80/−1; docs +28/−17;
design cycles 0 (light lane); fix cycles 0; implementer launches 1; owner touches 0.
**Booked forward:** 4b n1, n3 and `ci.c-abi.yml:25` → D9S-2. n2 (enum size on embedded toolchains;
C# should marshal as `int`) → domain 10 note; n4 harmless. Record: d9 `h:N` citations shifted (Mode
line noted).
**PM note:** the conformance lane needs the corpus emitted first (`emit_corpus.exe`) and `dotnet test`
run in `dotnet/BindingAbiConformance`; first attempt ran in the wrong folder and is not evidence.

## D9S-2 — A server build from the same sources — 🟢 2026-11-13

**Landed:** `0a43e99`, fixes `e676e1c`, `849b12d`. Conan `variant` option (edge default, in the
package ID; server+probe refused); per-target build constant (BC-10); server shim
`fletcher-c-abi-server` (owner ruling, Decision 1 (a)); `scripts/locate_shim.sh` picks shims by
package (T3, T5, T19 re-anchored); `scripts/check_shim_exports.sh` edge = server = header, probe as
negative control; CI wiring in `ci.c-abi.yml`, `ci.dotnet.yml`, `ci.pr.yml`. No Arrow yet.
**Steps:** 0a CLEAR · design `6108b90` · review APPROVE-WITH-DEBT(4) `fd6dcd2` · Decision 1 ruled · phase A
red (no `variant` option; forced server mapping "answered 1, linked to 2"; probe-as-server red by name) ·
0b CLEAR · green · 4b 0 blocking, S1 should-fix (taken) · 4a F1 blocking (CI/Linux unrun; marks
overclaimed) → fix 1 + PM narrowed marks · 4a cycle 2 C2-1 blocking (free name string) → fix 2 (name from
the variant again, empty-copy refusal kept) · 4a cycle 3 PASS · 4b cycle 2 clean.
**Verification:** both packages `conan create` run_tests (server 99/99, edge+probe 110/110), test_package
per package, export check 48=48=48, probe control and throwaway control (`fl_zz_throwaway`) red by name
(implementer logs `scratchpad/d9s2_conan.log`, `_fix1.log`, `_fix2.log`, PM-checked); full suite after
fix 2: binding-abi-conformance 11/11 (edge via `locate_shim.sh`), pubsub-arrow-fastdds 4/4 (PM).
**Not run:** every Linux leg and the CI workflows themselves (CI runs on pull requests only; opening a PR
is the owner's step). Marks read "Windows, by hand; CI and Linux unrun".
**Numbers:** production +339/−74, net +265 vs ceiling 250 (**close finding: over the ceiling**, from
the two review-required fixes; declared +197/−68, review forecast ~+250); tests ~+78/−15; design cycles
1; fix cycles 2; implementer launches 1 (resumed 3×); owner touches 1.
**Booked forward:** D9S-1 n1 → D9S-7; DEBT-1 → D9S-7. 4b nits logged (C2-N1, integration lanes'
first-match finds, double lookup, unquoted test_package path). Conan RECORD: `run_tests` does change the
package ID (comment in `conanfile.py` is wrong; pre-existing).

## D9S-3 — ejected from the light lane (2026-11-13)

At phase A the check `scripts/check_edge_arrow_free.sh` measured **59 non-comment lines** (brief
declared ~29); with phase B's workflow wiring (~+16) the stage lands near **75 touched lines vs the
60-line admission**. Ejected to the full loop: design doc, architecture review and compliance review
dispatched; phase A's script and red evidence carried forward unchanged (uncommitted).

## D9S-3 — The edge build carries no Arrow C++, checked by name — 🟢 2026-11-13

**Landed:** `52a0103`, fix `115b1e5`. `scripts/check_edge_arrow_free.sh`: graph half (`conan graph
info`, shipped edge configuration) and symbol half (`nm -C` Linux; `@arrow@@` scan Windows), refusals
when nothing was read, two controls that must go red by name every run; wired into `ci.c-abi.yml` on
both legs ahead of the size ceilings (DEBT-2). d9 L8 / BC-11 `[built]` (Windows by hand), Q4 resolved.
**Steps:** 0a CLEAR · light brief `e797564` · phase A (script; controls red) → **ejected** (~75 touched vs
60) · design `b0a6780` · review APPROVE-WITH-DEBT(3) `3dcfd69` (DEBT-3 applied before 0b) · 0b CLEAR ·
phase B wiring · 4a ISSUES(2, 0 blocking; F2 → owner ruling "Narrow the docs") · 4b 0 blocking, S1
should-fix taken → fix 1 → 4b cycle 2 clean.
**Verification:** Windows by hand: green on the real edge package (10 host nodes, no Arrow; 51
`@fletcher@@` names, no Arrow), graph control red naming `fletcher-arrow-bridge arrow`, symbol control
red (1248 Arrow names in the edge package's own `c_abi_tests.exe`); controls and refusals proven able to
fail (`scratchpad/d9s3_check.log`, `d9s3_fix1.log`). Linux: WSL with real `nm` on synthetic `.so`s and a
stub conan only. No compiled code changed, so D9S-2's suites stand. **CI and Linux unrun** (draft PR).
**Numbers:** production ~+115/−7 (62 non-comment in the script) in 3 files vs declared +106 in 2;
design cycles 1; fix cycles 1; implementer launches 1 (resumed 3×); owner touches 1.
**Logged:** 4b nits N1–N4 (no `--no-remote`; unknown extensions to `nm`; root counted; Windows scan
scope). Record: d9 L8 / design said `run_tests` leaves the package ID — false (flagged in place).
