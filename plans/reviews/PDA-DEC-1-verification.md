# PDA-DEC-1 — verification evidence

Branch `feature/protocol-driver-abi` at `a963211`. Full suite, this box, 2026-09-01.
Profile `Windows-msvc194-x86_64-Release`. Run delegated; table as reported.

| Component / harness | Result | From source | vs baseline |
|---|---|---|---|
| core | 28/28 | forced rebuild | none |
| arrow-bridge | 61/61 | forced rebuild | none |
| pubsub | 19/19 | forced rebuild | none |
| pubsub-arrow | 16/16 | forced rebuild | none |
| fastdds-pubsub-provider | 69/69 | forced rebuild | none (69 is the new baseline; one test retired this item) |
| xrcedds-pubsub-provider | 11/11 | forced rebuild | none |
| protoc | 99+3 | forced rebuild | none |
| pubsub-conformance | 36/36 | — | NEW this item |
| protoc-arrow-bridge | 90/91, 1 skipped (rustc absent) | — | none |
| protoc-coverage | 18/20, 2 gated skips | — | none |
| pubsub-arrow-fastdds | 3 of 4 under 28-way parallelism; **4/4 on -j1** | — | pre-existing cross-talk, characterization confirmed |
| fastdds-xrce-interop | 3/3 | — | better than baseline 1 (a live Agent was available; harness has 3 tests total) |
| gateway-end-to-end | 21/21 | — | none |
| gateway-fastdds-ts | 4/4, 4/4, 4/4 (three runs) | — | none; **no row loss in any run** |

## Two notes that matter more than the counts

1. **Every component package was cached from an earlier `run_tests=False` build and
   had to be force-rebuilt before its unit suite would run at all.** The
   `package_id()` cache-mask is live on this box, so any "green" claimed without
   confirming a source build is worthless. All counts above are post-force.
2. The conformance suite's forcing test, `ProviderConformance.SchemaBeforeDataAcrossHandoff`,
   is green on all five subjects. The design's falsification gate — clause 6 going
   red against a provider with reader-side data-sharing re-enabled — was **never
   met**; it is relieved by owner ruling 2026-09-01 and the defect is owned by
   PDA-ABI-7. See `integration-tests/pubsub-conformance/README.md` for the evidence.
