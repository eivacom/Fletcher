# D7F-1 — Test-authority audit, step 0b (new-test) — **CLEAR**

**Scope:** the uncommitted diff in `pubsub-arrow/tests/test_pubsub_arrow.cpp` (+137 lines, test:680-815): F1-F4, G1, helper `ExpectOneNullClosingReportOfAllRows`, plus its fixtures `NoTriggerBatchOptions`, `OrphanAttachments`, `PublishSchemaLessRows`, `PublishWellFormedRows`. Nothing else.
**Yardstick:** `plans/RIX-rulings.md` read top to bottom: 169 lines, 14 entries, last entry "2026-11-10 — Queued windows are delivered before the closing batch". Also d7 §3 L1-L14 and D9. `D7F-locked-decisions.md`, `D7F-plan.md` § D7F-1 and the design were used for context only.

| Test | Class | Authority it asserts |
|---|---|---|
| F1 `NoUsableSchemaDropsReportedAtUnsubscribe` | LAWFUL | L1 "A lost row is always reported, never silent"; L7 "never 'no rows'"; L9 "The pending window is delivered once, as `kClosing`"; L3 "A dropped row takes its attachment with it"; D9 "no usable schema has arrived yet" |
| F2 `...AtDestruction` | LAWFUL | the same laws, with `~SubscriberArrow` as the ending (closes the d7 L9 gap "The closing flush from the destructor has no test") |
| F3 `SchemaDeclaredAfterSubscribeDropsReportedAtUnsubscribe` | LAWFUL | as F1. Note: its premise (a subscription made before the declaration keeps a null schema under InProcess `as_declared`) is domain 3 / spec behaviour (tier 2), not a law. It is the declared ladder variant, so this is not a finding. If domain 3 ever forwards late declarations, F3 changes, and the reason is the premise, not D7F. |
| F4 `...AtDestruction` | LAWFUL | as F3 |
| G1 `NoUsableSchemaIdleSubscriptionDeliversNothingAtClose` | LAWFUL guard | L6 "A window with nothing pending delivers nothing"; L7. It requires a state that tier 1 *requires*, not one it forbids. |
| Helper `ExpectOneNullClosingReportOfAllRows` | LAWFUL | carries the assertions above. Exactly one delivery = L9 "once". |

**(a) Forbidden or tolerated behaviour:** none. No test asserts or accepts a nested `kClosing`, overlapping callbacks (L12), a null batch that means "no rows", or a silent drop.
**(b) Driven from outside:** yes. Tests use only `PublisherArrow::Publish/PublishDirect/CreateTopic` and `SubscriberArrow::Subscribe/Unsubscribe/~` against the Mock or `InProcessPubSubProvider`, and observe only the user callback (`BatchSink`). `RecordBatchBatcher` is never touched.
**(c) New value subject:** none. All five tests are ladder rung 1 and the design's F1-F4 and G1 (the InProcess variant is named in the ladder). No stop-and-ask.
**Mechanism pinning against D7F-2 and D7F-3:** none found.
- With `timeout = 10 min` and `max_rows = 100000`, a D7F-2 deadline report cannot fire inside the test, so "exactly one delivery" does not pin reporting only at close.
- No assertion reads thread identity.
- Reading results right after an outside `Unsubscribe` or destructor stays lawful under L9 and ladder rung 4 (everything is delivered before the call returns).
- No `WaitFor` is needed, and none of these tests is among the 10 that D7F-3 re-anchors.

**Scheduled restoration:** none. **Tier-1 disagreement:** none.
**Laws with no test (outside this scope, for the record):** L5 timing, L10 throwing callback, L12 (D7F-3), L13 batched. L14 is `[designed-not-built]` and correctly has no test.
