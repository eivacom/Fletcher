# D9S — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D9S-1 | `BindingAbi.TheVariantIsAnsweredBeforeAnyOtherCall`, `.TheVariantAndVersionAnswerAlikeFromManyThreads` (+ `probe.*`) — red: compile, query absent; green | c-abi `conan create` run_tests+probe 110/110 (implementer, PM-checked); C# NativeLoaderTests control 11/11 at 8, red at 7 | binding-abi-conformance 11/11; pubsub-arrow-fastdds 4/4 (PM) | `fcb7be6` |
| 2026-11-13 | D9S-1 | `TheVariantIsAnsweredBeforeAnyOtherCall` | fcb7be6 |
| D9S-2 | `c-abi/test_package` per package (server answers server, edge answers edge); re-anchored `BindingAbi.TheVariant*` per artefact; `check_shim_exports.sh` 48=48=48 with probe and `fl_zz_throwaway` controls red by name | server 99/99, edge+probe 110/110 (implementer, PM-checked) | binding-abi-conformance 11/11, pubsub-arrow-fastdds 4/4 (PM); **CI and Linux legs unrun** (PR-only) | `0a43e99`, `e676e1c`, `849b12d` |
| 2026-11-13 | D9S-2 | `TheVariantIsAnsweredBeforeAnyOtherCall` | 849b12d |
