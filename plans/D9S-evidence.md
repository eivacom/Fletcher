# D9S — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D9S-1 | `BindingAbi.TheVariantIsAnsweredBeforeAnyOtherCall`, `.TheVariantAndVersionAnswerAlikeFromManyThreads` (+ `probe.*`) — red: compile, query absent; green | c-abi `conan create` run_tests+probe 110/110 (implementer, PM-checked); C# NativeLoaderTests control 11/11 at 8, red at 7 | binding-abi-conformance 11/11; pubsub-arrow-fastdds 4/4 (PM) | `fcb7be6` |
| 2026-11-13 | D9S-1 | `TheVariantIsAnsweredBeforeAnyOtherCall` | fcb7be6 |
