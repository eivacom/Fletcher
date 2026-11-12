# D7O-3 — test-authority audit, step 0a (corpus)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test in scope. Tree `f587e36`.

**Proof of read:** `plans/RIX-rulings.md`, 295 lines, 24 entries, last heading "2026-11-12 — D7O-2: the deadline definition says some windows have none". d7 §3 L1-L15 read; ladder rung 4 + "Tests other than rungs"; `plans/D7O-locked-decisions.md` D-D7O-1..6; register BC-8 (`bounded-context-register.md:22`).

**Yardstick:** L15 "Every value it accepts is used as given"; L8 "An undecodable schema is permanent" (only undecodable/unimportable); ruling 2026-11-12 "a failed pre-size never makes a decodable schema unusable"; D-D7O-3 "refused, never clamped".

**Scope covered:** all 38 `SubscriberArrowBatchTest.*` in `pubsub-arrow/tests/test_pubsub_arrow.cpp` with helpers `NoTriggerBatchOptions`/`DeadlineBatchOptions` (max_rows 100000), `RowLimitOptions`, `RunAssemblyCell`; `test_pubsub_arrow_heavy.cpp` (2 tests, max_rows 1000); `pubsub-arrow/tests/discard_probe.cpp` (nodiscard negative-compile TU; no max_rows/Reserve); `arrow-bridge/tests/test_batch_decoder.cpp` (no `BatchDecoderTest` touches `Reserve`); `integration-tests/pubsub-arrow-fastdds/tests/test_roundtrip.cpp:244` (max_rows 3); `arrow-bridge/benchmarks/bench_arrow_codec.cpp` (Reserve(n), n in {1,1000,8000}). No DISABLED_/GTEST_SKIP/declared-red rows found.

**Findings:**
- No test passes a `max_rows` whose pre-size fails; the largest is 100000. Nothing asserts or tolerates "decodable schema -> null for life". No test pins a clamp (D7O-1 removed it; `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` asserts refusal, LAWFUL).
- `NestedDictionarySchemaReportsEveryRowDropped`, `EveryAssemblyStateReportsItsDropsAtEitherEnding` (undecodable/unimportable cells): LAWFUL under L8; their schemas are truly undecodable/unimportable, not pre-size failures. Not adjacent to the forbidden case; keep as-is.
- `FinishFailureIsReportedNotFatal`: LAWFUL, and supports L8's boundary (decoder reset, next window decodes).
- `bench_arrow_codec.cpp:388-390` comment describes the "Reserve(max_rows_) after Finish()" mechanism: not an assertion; may go stale if D7O-3 bounds the pre-size. Note for the implementer only.
- No scheduled restoration found. No tier-1 disagreement.

**Laws without tests:** L15's "used as given" for a huge `max_rows` (`[designed-not-built]`, owed by rung 4) and the BC-8 sweep (owed by D7O-3), as expected. L10 throwing callback and L14 remain untested (out of D7O-3).
