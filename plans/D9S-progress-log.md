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
