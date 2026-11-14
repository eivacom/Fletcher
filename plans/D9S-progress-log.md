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

## D9S-4a — Batches cross the C ABI on the server build; the edge build refuses — 🟢 2026-11-13

**Landed:** `e766c3e`. Header fixes `8383095` and `e57e7af`; reviews in `609ee23`.
- **Server build:** `fl_subscriber_subscribe_batches` hands every window that domain 7 closes to the caller as an importable Arrow batch, with its reason and loss count. A window that could not be built arrives as no-batch, with its rows counted.
- **Edge and probe builds:** refuse with not-supported.
- **Scan:** the BC-9 scan was widened.

**Steps:**
- Design: 2 cycles, APPROVE-WITH-DEBT(3). Ruled on: PIC (fPIC on the two batcher-side recipes), one subscriber object, the ceiling overrun.
- Test audits: 0a CLEAR. 0b CLEAR in cycle 2, post-build (the tests arrived in the code commit).
- Compliance review 4a: ISSUES(2, 0 blocking). F2 is the rung-4 test reshaped to ROW, ROW, CLOSING.
- Code review 4b, cycle 1: 2 should-fix.
  - **S1 DECLINED by the PM.** The fix would refuse a batched id in the row unsubscribe. d9 L13 forbids changing what an edge entry point does, and this round may change no laws. The no-op is documented in that function's own doc instead.
  - **S2:** header text on the destroy contract → fix 1.
- Code review 4b, cycle 2, **B1:** fix 1's text promised a CLOSING batch, but an idle last window delivers nothing (d7 L6) → fix 2.
- Code review 4b, cycle 3: CLEAR. The review was then cut from 162 to 102 lines to meet the cap.

**Verification at `e57e7af`:**
- c-abi tests: server 104/104, edge+probe 114/114. The forcing tests are green on server, and the edge answers with a refusal.
- Conformance: edge 11/11.
- **Server conformance: 1/11 under the server DLL's own file name**, because the C# binding loads `fletcher-c-abi` by name. The same server DLL scores 11/11 when staged under the edge name. This is expected: the file name a binding loads is domain 10's Q5a, and D9S-7 owns "the lane passes against both builds".
- pubsub-arrow-fastdds 4/4.
- CI and Linux unrun (draft PR). Logs are in the session scratchpad.

**Numbers:**
- Production: +415/−20 in 12 files (code ≈+305), against a declared +298/−12. The overrun was ruled (`7b1bcbe`).
- **Close finding:** tests came to +646 against a declared ~270, and the raw diff is about +67% over the declared total, with no owner note.
- Cycles: design 2, fix 2. Implementer launches: 2 in the fix stage (launches during the build stage went unrecorded). Owner touches: 3 rulings.

**Booked forward to D9S-5 (rung 6):** after a destroy from inside a batch callback, the C caller has no reliable end signal for `ctx`. CLOSING is not delivered when the last window is idle. The header now tells bindings not to destroy from inside a batch callback, or to keep `ctx` alive for the life of the process. D9S-5's design must face this gap, which may become an owner question.

**Logged:**
- 4b nits N1–N8.
- 0b: rungs 6 and 7 are partly covered by the rung-4 test, but must not be marked green on it.
- Suite agent: `locate_shim.sh` refuses when the cache holds several recipe revisions, and a stale shim in the `bin` folder could have produced a silent false green.

**Owner, 2026-11-13 (words-per-stage tripwire, about 7,300 vs 6,520):** "Tighter caps (Recommended)". The design cap is now 150 and the review cap 60 for the remaining full-loop stages; config and ledger updated.

## D9S-4b — Each batch row carries its own attachments to the C caller — 🟢 2026-11-13 (light lane)

**Landed:** `b09fc64`, with the record fix in `6b7b29c`. `fl_batch` gains `attachments`, appended at the end of the struct. It holds `num_rows` sets, none of them NULL, with an empty set for a row that has no attachments. The list itself is NULL exactly when the batch has 0 rows. The thunk moves domain 7's per-row vector into a deque of `fl_attachments`, borrowed for the duration of the callback. The shim adds no check of its own (L15).

**Steps:**
- 0a CLEAR.
- Brief `023152b`, with no owner decisions.
- Phase A: red, at compile time and at runtime.
- 0b CLEAR. Its note 2 was taken: 4a's no-batch test now asserts a NULL list.
- Phase B: green.
- Code review 4b: 0 blocking, 2 nits (reserve the pointer list; an out-of-memory leak shared with the row path).

**Verification:** c-abi server 105/105 and edge+probe 114/114. Conformance 11/11 on edge and on a renamed copy of the server shim. pubsub-arrow-fastdds 4/4. CI and Linux unrun.

**Numbers:** 12 touched non-comment production lines in 2 files, against a declared ~13. Design cycles 0, fix cycles 0, implementer launches 1. Owner touches 0.

**Recorded:**
- BC-13 gained no attachments clause, because its text never named attachments; the brief's line was wrong.
- d9 L14's stale `h:1061-1066` pointer was corrected to `h:1180-1183`.
- The suite agent's `--help` misuse created a stray directory, which it removed.

## D9S-5 — 0a BLOCKED → owner ruling; split into 5a and 5b (2026-11-14)

0a (`plans/reviews/D9S-5-audit-0a.md`): BLOCKED, MUST-DELETE(0). The 2026-11-11 ruling ("then the closing batch") collides with d7 L6 ("a window with nothing pending delivers nothing") on an idle close. Owner: "Always a final delivery (Rec.)" and "Split and proceed (Rec.)" (ledger 2026-11-14). The round is widened to d7 L6 (closing only) in D9S-5b; config updated. Tracker: D9S-5 → 5a (move/release, options, close from outside a callback) and 5b (the closing delivery always arrives, including in-callback and idle; the d7 change). Texts still to correct: ladder rung 6 (`D9S-test-ladder.md:61`) and the d9 L1 annotation, both in 5b. The two d7 tests that pin the old L6 empty-close behaviour (`test_pubsub_arrow.cpp:1374`, `:1414`) are re-audited at 5b's 0a.

## D9S-5a — Batched close; a batch is the caller's only if moved; options reach domain 7 as given — 🟢 2026-11-14

**Landed:** `ea49aa3`, then fix `4cb0144` and record fix `640e8cd`.
- `fl_subscriber_unsubscribe_batches` sits behind one `RequireServerBuild` door, which the subscribe now shares.
- An untagged id or a NULL subscriber gets `FL_INVALID_ARGUMENT`. d9 §4 records this as tier 3: no owner ruling exists for it.
- `~fl_subscriber` detaches the batch host before teardown (S1).

**Steps:**
- 0a: the D9S-5 audit (BLOCKED, resolved by the 2026-11-14 ruling) stood for 5a.
- Design: cycle 1, APPROVE-WITH-DEBT(4). DEBT-3 (brief at its cap) was handled by the PM trimming the placeholder.
- Phase A: red. The implementer reverted a stub that my brief allowed and **d9 L6 forbids**. That was a PM error, and the law governed.
- 0b: CLEAR.
- Green.
- 4a: PASS, 5 RECORD items. 4b: APPROVE, 1 should-fix (S1, the race between destroy and an in-callback close).
- Fix 1, then 4b cycle 2: APPROVE.
- Record fixes: d9 §1/L4 and BC-10 now name the close's refusal; the stale not-built marks were corrected.

**Verification:**
- c-abi server 109/109 and edge+probe 116/116 at `4cb0144`; exports 50=50=50.
- Full suite at `ea49aa3`: conformance edge 11/11 and server shim (renamed) 11/11; pubsub-arrow-fastdds 4/4.
- Not re-run after fix 1: it changes only the destructor, which neither lane touches.
- G1 and G2 mutation controls went red; they are recorded in the evidence.
- CI and Linux unrun.

**Numbers:** production +104/−17 raw in 5 files (55 touched, against ≈89 declared); tests +547/−8 (1.12×). Design cycles 1, fix cycles 1, implementer launches 1 (resumed 3×). Owner touches: 1 ruling pair (idle close, split).

**Logged nits:**
- 4b: G1's provider-order loop asserts nothing new.
- 4b cycle 2: a subscribe from a batch callback during destroy makes a second host. It is safe; the door could refuse it in 5b.
- Suite agent: the cache now holds two edge packages, so `locate_shim.sh` refuses until one is removed.

**D9S-5b, 2026-11-14:** design review cycle 1 NEEDS-REWORK (B1: a refused subscribe can deliver). Cycle 2 framed it as Decision 1. Owner: "(a) Never (Rec.)" (ledger). The round is widened by the d7 L1 refused-subscribe clause; config updated.
