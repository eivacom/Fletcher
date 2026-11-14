# D9S-4 — step 0a test-authority audit (MODE=corpus)

**Verdict: CLEAR.** Tree `8854773`. No test in scope asserts or tolerates behaviour a tier-1 authority forbids. Nothing is scheduled for restoration, and there is no tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md` was read top to bottom. It has 431 lines. The last heading (l.421) is "2026-11-13 — D9S-3: a test-only Arrow dependency of the edge build is refused too". I also read: d9 §2–§7 (L1–L18, D1–D31, Q6/Q7/Q12), d7 §3–§6 (L1–L15, D1–D17), register BC-8–BC-15, the ladder (rung 4 in full) and the locked decisions D-D9S-1..8.

**Scope covered (all LAWFUL unless noted):**
- `c-abi/tests`: `test_binding_abi.cpp` (4 rows, also run as `probe.*`), `test_binding_entry_points.cpp` (54), `test_containment.cpp` (6), `test_nanoarrow_codec.cpp` (29), `test_probe_shim.cpp` (7), `test_single_copy.cpp` (5), `c99_probe.c`, `single_copy_decoy.c`, `codec_corpus.hpp`, `tests/CMakeLists.txt` (per-artefact `FLETCHER_EXPECTED_VARIANT`). There are no disabled or skipped rows.
- `c-abi/test_package` (the `example.c` variant/version check per package).
- `scripts/check_shim_exports.sh` (edge = server = header; probe control), `scripts/check_edge_arrow_free.sh` (graph is `variant=edge` only; two controls) and `scripts/locate_shim.sh`. With the server build linking Arrow, both checks stay lawful and must stay green. The Linux "nothing outside fl_*" arm also binds the server shim, and L7 requires exactly that.
- CI ceilings in `ci.c-abi.yml`: the edge ceiling is enforced, and the server shim's size is printed but not enforced (D22, R-9 are domain 10's). `check_shim_dependencies.sh` runs on the edge shim only.
- `integration-tests/binding-abi-conformance`: `emit_corpus.cpp`, plus `CorpusConformanceTests.cs` with 2 theories and 1 fact.
- `pubsub-arrow/tests` (read-only): the 37 `SubscriberArrowBatchTest` rows and the 2 heavy rows. They pin `PubSubError(kInvalidArgument)` for L15, which d9 L15's pass-through depends on.

**FL_NOT_SUPPORTED pins (D28 widened the meaning):** three tests assert it. They are `TheSelectorIsClassifiedBeforeItIsResolved` (:822), `ASchemaWatchOnATransportWithoutASchemaChannelIsNotSupported` (:1486) and `AProviderWithNoNotionOfAFieldRefusesItAsNotSupported` (:1922). All three are provider or path cases, which the widened meaning still covers. None asserts that the status is *only* about providers, so none blocks the edge refusal. Prose only: the comment at `test_binding_entry_points.cpp:1487-1488` ("means … THIS TRANSPORT has no … channel") and the one at `tests/CMakeLists.txt:8-13` ("`fletcher-c-abi` must never link `arrow-bridge`") are now narrower than D28 and L8 (edge only). Touch them up if the stage edits those lines. They are not test findings.

**Notes, not violations:**
- `DictionaryColumnRefoldedToDictionaryArray` pins domain 2's re-fold. A C batch caller will see dictionary arrays where `fl_decode_rows` gives the value type (d9 Q11, open, owned by domain 2/10). Rung 4's value-for-value comparison should avoid dictionary columns, or state which form it expects.
- Laws with no test: d9 L2 (enum equality), L6 (append-only), L1 (every entry point with a throwing body); d7 L10 (throwing callback). L14/L15/L18 and the L12/L13 batched halves are `[designed-not-built]` with no test, so there are no false greens. d9 L3's "no test asserts OUT untouched" is partly stale: two rows assert `arrival == nullptr` after a refusal.
