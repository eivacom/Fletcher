# D10C-3 step 0a: corpus audit (rung 3, per-row SubscriberArrow on the native path)

**Verdict: CLEAR.** No test in scope asserts or accepts a per-row bad row thrown, skipped or uncounted.

**Proof of read:** `plans/RIX-rulings.md`, 603 lines, 45 entries, last "## 2026-11-15 — D10C ladder: the domain-7 port tests" (:595).
**Yardstick:** d10 §1, L3, L7, L9, L11 (per-row clauses); register BC-16, BC-2; rulings :246 (2) as amended by :536 (1), :363 (4), :551 (4), :575 (3) "An undecodable per-row Arrow row arrives as a null batch with RowsDropped = 1", :575 (5), :575 (6).

**Scope (HEAD e770211; no `Skip`, `[Ignore]` or `#if false` anywhere in `dotnet/tests` or `integration-tests`):**
- `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`, the rung's 12: T1 :146, T2 :164, T3 :185, T4 :201, T5 :218, T8 :275, T9 :297, T10 :314, T11 :337, T29 :823, T32 :888, T41 :1191. All LAWFUL, all RE-ANCHOR. T32 and T41 have not moved since D10C-1's 0a.
- The same file's other per-row caller, T7 :258 (both forms). LEGITIMATE-BUT-ADJACENT: today both halves get `NotSupported` from `inprocess`. The batched half turns into tolerance only at rung 7, which already re-anchors it.
- Fixtures: `Sink` and `Delivery` :1240-1300, constructor and helpers :86-138. LAWFUL. `Delivery.Rows` = -1 on a null batch, so it cannot hide a null drop.
- Batched bad-row tests (on the managed path until rung 7): T13 :391, T18 :487, T19 :511, T22 :583, T24 :669, T27 :765, T28 :801. Each one counts its drop in `RowsDropped`, with a null batch where no decoder exists. None is per-row. LAWFUL under d7 L7/L8 and :258 (3).
- Per-row handler-throw path: T41, plus `ProcessWideTests.AbsorbedTotalMovesByExactlyWhatTheSubscribersAbsorbed` :165 (on `Subscriber`). LAWFUL under L9 ("caught at its thunk, counted and raised").
- `integration-tests/pubsub-transport-conformance/.../TopicOptionsOverFastDdsTests.cs:352` `ASubscriberArrowForwardsItsReaderProfileOnBothForms`. Its per-row half is LAWFUL. **Neither aud part C nor the ladder lists it:** its search glob `integration-tests/*dotnet*` misses this path. Its batched half needs the server leg from rung 7 on. The ladder must place it.

**The :575 (3) check:** no existing test sends a malformed, schema-less or unopenable row through per-row `Subscribe`. Every bad-row test uses `SubscribeBatched`. So no test asserts an exception, a silent skip or an uncounted drop for a per-row bad row, and none asserts the ruled outcome either.

**UNMOORED (report only):**
- T8 :287 asserts `BatchReason.RowLimit` on a per-row delivery. No law sets the per-row reason (the ladder leaves it to the architect), so the test pins the `MaxRows = 1` vehicle.
- T29 :833 relies on `own.Subscription.Id == foreign.Subscription.Id`, which is per-instance id numbering. It must still hold on `fl_subscriber_subscribe`, or the test proves nothing.

**Laws vs build marks:**
- No test exists for L7/L9's per-row-Arrow clause (BC-16). That is expected: it is `[designed-not-built]` and is the rung's red.
- L11 is `[built for row deliveries]`, but `SubscriberArrow.Dispose`'s refusal (`SA:310-316`) has no test (d10 F3). The rung's declared guard closes this.
- No false green. No scheduled restoration of a forbidden subject. No tier-1 disagreement.
