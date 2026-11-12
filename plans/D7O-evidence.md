# D7O — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D7O-1 | `SubscriberArrowBatchTest.OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` (forcing; red at phase A: 24 assertions, no throw), guard `.InRangeBatchOptionsAreUsedAsGiven` — green | pubsub-arrow `conan create` run_tests 52/52 (implementer, PM-checked log, 2026-11-12) | `ctest -L heavy` 2/2; fastdds provider test package PASS; pubsub-arrow-fastdds 4/4 (PM, 2026-11-12) | `9abb0c7` |
| 2026-11-12 | D7O-1 | `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` | 9abb0c7 |
| D7O-2 | `SubscriberArrowBatchTest.NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing` (red: compile, no `kNoTimeout`), `.TimeoutTooLongForTheClockBehavesAsNoTimeout` (red: early `kTimeout`, 2 arms), control `.ShortTimeoutControlSeesTheDeadlineInEveryBody` — green | pubsub-arrow `conan create` run_tests 55/55 (implementer, PM-checked log, 2026-11-12) | `ctest -L heavy` 2/2; pubsub-arrow-fastdds 4/4 (PM, 2026-11-12) | `7a0cc64`, `6e5cd89` |
| 2026-11-12 | D7O-2 | `NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing` | 6e5cd89 |
| D7O-3 | `SubscriberArrowBatchTest.MaxRowsLargerThanAnyWindowIsUsedAsGiven` (red: null batches, all rows dropped), guards `.AFailedPreSizeDoesNotMakeTheSchemaUnusable`, `.EveryBatchOptionIsRefusedExactlyWhenOutOfRange` (BC-8, 42 cells) — green | pubsub-arrow `conan create` run_tests 58/58 (PM run at `c18e95e`; implementer after fix `57aa4bb`, PM-checked log) | `ctest -L heavy` 2/2; pubsub-arrow-fastdds 4/4 (PM, at `c18e95e` and `57aa4bb`) | `c18e95e`, `57aa4bb` |
| 2026-11-12 | D7O-3 | `MaxRowsLargerThanAnyWindowIsUsedAsGiven` | 1434325 |
