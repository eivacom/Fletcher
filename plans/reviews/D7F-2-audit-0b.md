# D7F-2 — Test-authority audit, step 0b (new-test)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test. Nothing schedules a restoration. No tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md`, 169 lines, 14 entries; last: "2026-11-10 — Queued windows are delivered before the closing batch". Yardstick: d7 §3 L1–L14, D1–D11; `plans/D7F-locked-decisions.md`.

**Scope:** the uncommitted diff to `pubsub-arrow/tests/test_pubsub_arrow.cpp` (+240): 4 tests plus helpers `kDeadlineTimeout`, `DeadlineBatchOptions`, `ExpectNullTimeoutReport`, `ExpectLateSchemaRowsAssembled`, `AssemblyState`/`Ending`, `RunAssemblyCell`. It also reuses D7F-1's helpers (`BatchSink`, `NoTriggerBatchOptions`, `PublishSchemaLessRows`, `PublishWellFormedRows`), which are unchanged.

| Test | Class | Basis |
|---|---|---|
| `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` (forcing) | LAWFUL | L1, L5, L6 (the idle check), L7 (null), L13 (a late schema assembles), L2 (Σ = 7). An *awaiting* schema is not an *undecodable* one, so L8 is not engaged. |
| `SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` (forcing) | LAWFUL, with one UNMOORED premise | L1, L6, L7. The null batches after `CreateTopic` depend on the in-process provider holding on to the schema it had at `Subscribe` (design P2). No law owns that rule. The comment disclaims it, as audit 0a asked. If the provider changes lawfully, this test fails, and the design already makes that a STOP-AND-ASK. |
| `EveryAssemblyStateReportsItsDropsAtEitherEnding` (guard) | LAWFUL | L1, L6, L7, L8. It requires no forbidden state. Each of the 6 cells requires exactly one report, with `rows_dropped` 1. |
| `SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops` (leg) | LAWFUL | L7 ("never 'no rows'": a batch could be built, so it is non-null), L2 (2 + 3), L3 (attachments aligned). |

**(b) Driven from outside:** yes, in all four tests. They use only `PublisherArrow` (`Publish`, the public `PublishDirect`, `CreateTopic`) and `SubscriberArrow` (`Subscribe`, `Unsubscribe`). They read the callback sink and nothing internal.
**(c) New value subject:** none. The tests follow the item subject (deadline report, late schema, conservation across the change) and the ladder's states × endings. The leg's mixed-window shape is what L7 requires, not a new choice.
**(d) What D7F-3 must change:** nothing is pinned. Every read goes through `WaitFor` or comes after an outside `Unsubscribe`, which D7F-3 keeps (closing delivers the queued windows, then `kClosing`, before it returns). No test asserts which thread delivers or that delivery happens synchronously after `Publish`. Cross-delivery order is deliberately not asserted (`ExpectLateSchemaRowsAssembled`); order within one batch is order within a window, which is lawful. W1 and W2 are ordered by `WaitFor`, not by L12. No test depends on drops counting towards `max_rows`.
**Nit, not a finding:** the in-process test uses `kSchemaLessRows` as the count of `PublishWellFormedRows` (both are 3, and the helper's comment says so).
**Laws without a test after this stage** (outside this scope): L1 violation 2, L4's budget flush, L5's timing, L10's throwing callback, L12, L14 (designed-not-built).
