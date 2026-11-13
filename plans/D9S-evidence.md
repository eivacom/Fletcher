# D9S — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D9S-1 | `BindingAbi.TheVariantIsAnsweredBeforeAnyOtherCall`, `.TheVariantAndVersionAnswerAlikeFromManyThreads` (+ `probe.*`) — red: compile, query absent; green | c-abi `conan create` run_tests+probe 110/110 (implementer, PM-checked); C# NativeLoaderTests control 11/11 at 8, red at 7 | binding-abi-conformance 11/11; pubsub-arrow-fastdds 4/4 (PM) | `fcb7be6` |
| 2026-11-13 | D9S-1 | `TheVariantIsAnsweredBeforeAnyOtherCall` | fcb7be6 |
| D9S-2 | `c-abi/test_package` per package (server answers server, edge answers edge); re-anchored `BindingAbi.TheVariant*` per artefact; `check_shim_exports.sh` 48=48=48 with probe and `fl_zz_throwaway` controls red by name | server 99/99, edge+probe 110/110 (implementer, PM-checked) | binding-abi-conformance 11/11, pubsub-arrow-fastdds 4/4 (PM); **CI and Linux legs unrun** (PR-only) | `0a43e99`, `e676e1c`, `849b12d` |
| 2026-11-13 | D9S-2 | `TheVariantIsAnsweredBeforeAnyOtherCall` | 849b12d |
| D9S-3 | `scripts/check_edge_arrow_free.sh` green on the real edge package; graph control red naming `fletcher-arrow-bridge arrow`; symbol control red (1248 Arrow names, edge package own `c_abi_tests.exe`); refusals proven | n/a (no compiled code changed; D9S-2 suites stand) | Windows by hand; **CI and Linux unrun** (PR-only) | `52a0103`, `115b1e5` |
| 2026-11-13 | D9S-3 | `check_edge_arrow_free` | 115b1e5 |
| D9S-4a | `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder`, `.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted`, `.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` green on server; edge answers with a refusal; BC-9 scan + control | server 104/104, edge+probe 114/114 at `e57e7af` (implementer, PM ran the forcing tests) | binding-abi-conformance edge 11/11; server shim 11/11 **only when staged under the edge file name** (the binding loads by name: Q5a / D9S-7), 1/11 under its own name; pubsub-arrow-fastdds 4/4; **CI and Linux unrun** | `e766c3e`, `8383095`, `e57e7af` |
| 2026-11-13 | D9S-4 | `EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` | e57e7af |
| D9S-4b | `Batches.EachBatchRowCarriesItsOwnAttachments` (red: runtime, NULL list; green) + NULL-list assertion in the 4a no-batch test | server 105/105, edge+probe 114/114 at `b09fc64` | binding-abi-conformance edge 11/11, server shim renamed 11/11; pubsub-arrow-fastdds 4/4; **CI and Linux unrun** | `b09fc64` |
| 2026-11-13 | D9S-4b | `EachBatchRowCarriesItsOwnAttachments` | 6b7b29c |
