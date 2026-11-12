# D7O — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D7O-1 | `SubscriberArrowBatchTest.OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` (forcing; red at phase A: 24 assertions, no throw), guard `.InRangeBatchOptionsAreUsedAsGiven` — green | pubsub-arrow `conan create` run_tests 52/52 (implementer, PM-checked log, 2026-11-12) | `ctest -L heavy` 2/2; fastdds provider test package PASS; pubsub-arrow-fastdds 4/4 (PM, 2026-11-12) | `9abb0c7` |
| 2026-11-12 | D7O-1 | `OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind` | 9abb0c7 |
