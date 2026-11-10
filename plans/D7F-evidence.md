# D7F — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D7F-1 | `SubscriberArrowBatchTest.NoUsableSchemaDropsReportedAt{Unsubscribe,Destruction}`, `.SchemaDeclaredAfterSubscribeDropsReportedAt{Unsubscribe,Destruction}` — green; red before the fix (0 deliveries vs 1), reported by the implementer at phase A | pubsub-arrow 39/39 passed (PM run, forced rebuild, 2026-11-10) | pubsub-arrow-fastdds 4/4 (implementer, 2026-11-10) | `a2c8b97` |
| 2026-11-10 | D7F-1 | `NoUsableSchemaDropsReportedAtUnsubscribe` | a2c8b97 |
