# D7O-2 — step 0b test-authority audit (MODE=new-test)

**Verdict: CLEAR.** Base `920b3ec`; scope = uncommitted D7O-2 section of `pubsub-arrow/tests/test_pubsub_arrow.cpp` (2500-2774): `NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing` (rung 2), `TimeoutTooLongForTheClockBehavesAsNoTimeout` (rung 3, 100-year guard arm), `ShortTimeoutControlSeesTheDeadlineInEveryBody` (control), helpers `BodyRun`/`Body`, `Observe`, `Count`, `Describe`, `XAt`, `ExpectBodyAClosesOnlyWhenFullOrAtClosing`, `ExpectOnlyTheEndingReportsTheLostRows`, `WaitForTimeoutDelivery`.
**Yardstick:** `plans/RIX-rulings.md` read in full: **295 lines, 24 entries, last "2026-11-12 — D7O-2: the deadline definition says some windows have none"**; d7 §3 L1-L15, D14, D15.

| Test | Class | Basis |
|---|---|---|
| Rung 2 (4 arms) | LAWFUL | L4 "closes at ... the deadline (only for a subscription that has a timeout; L5)"; L5 "its windows have no deadline and close only on row count, the offset budget or closing"; losses reported in the `kClosing` batch satisfy L1 "counted in the `rows_dropped` of a later delivered batch" and L6; nothing after closing = L9; null/no-attachments = L7, L3. No loss left unreported. |
| Rung 3 (3 arms) | LAWFUL | L5 "a timeout too long for the clock to represent means no deadline, never an early or wrapped one" (confirmed, ruling LC-1). 100-year guard: L5 "never earlier than its timeout says" + L15 "Every value it accepts is used as given" (forbids refusing huge values). |
| Control (10 ms) | LAWFUL | Asserts some `kTimeout` within 2 s, that it holds ≥1 row (a) / ≥1 drop (b, c), and conservation (L2). The content check is entailed by L6 "A window with nothing pending delivers nothing" + body (a) having no drops; no row identity is asserted (DEBT-1 honoured). Asserts no closing batch and no cadence, so nothing L5/L6 forbid for a finite timeout. |
| Helpers | LAWFUL | Public path only (`SubscriberArrow`, `PublisherArrow::Publish/PublishDirect`, `MockProvider`, `BatchSink`); no `RecordBatchBatcher` access. Sink declared before subscriber. |

**UNMOORED sub-assertion (not a violation):** rung 2 (b)/(c) pin "lost rows never count toward `max_rows`" (5 drops, `max_rows` 2, nothing before closing). No tier-1 text says whether L4's "`max_rows` rows" includes drops; it is backed only by D-D7O-5 / audit F2 (tier 3) and today's code. If the owner ever rules drops count, these arms are the ones to revisit.

**Timing:** every "nothing delivered" check is one-sided over 200 ms; upper bounds are only the 2 s liveness `WaitFor` convention; post-ending counts rely on L9's waiting `Unsubscribe`/destructor from outside a callback (deterministic). Not flaky.
**Value subjects:** only rungs 2-3 values (kNoTimeout, `ms::max()`, clock max in ms, 100 y, 10 ms; `max_rows` 2/3). No `INT64_MAX max_rows` arm — rung 4 not pulled forward.
**Scheduled restoration:** none. **Tier-1 disagreement:** none (L5 and the §2 Deadline ruling agree; d7 §2 still shows the old text, a tier-3 edit D7O-2 owes).
**Build-state:** L4/L5 no-timeout and unrepresentable cases are `[designed-not-built]` and these tests are red/uncompiled — no false green.
