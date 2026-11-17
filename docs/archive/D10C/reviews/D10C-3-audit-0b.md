# D10C-3 step 0b: new-test audit (rung 3, per-row SubscriberArrow on the native path)

**Verdict: CLEAR.** None of the new tests asserts or accepts anything a tier-1 authority forbids. No test brings in a new value subject beyond rung 3. Nothing schedules a restoration. No tier-1 disagreement.

**Proof of read:** `plans/RIX-rulings.md`, 603 lines, 45 entries, last heading "## 2026-11-15 — D10C ladder: the domain-7 port tests" (:595).
**Yardstick:** rulings :536 (1), :575 (3) "An undecodable per-row Arrow row arrives as a null batch with RowsDropped = 1", :575 (5) "Per-row and batched may deliver different Arrow types for one dictionary column; C# converts nothing", :484/:500 (closing applies to *batched* subscriptions only), :258 (3); d10 L2, L7 ("adds no window, count or timer"), L9, L11, D20, D30, D32; register BC-16 ("every handler call carrying exactly one row or one drop"); ladder rung 3 and its ground rules.
**Scope (uncommitted diff against 5b311af, `SubscriberArrowTests.cs` +294):** the forcing Theory at :406 (all three arms, plus the fixtures `RowKind`, `Bytes` and `DictionaryOfStructs`), the guard at :540, the guard at :578, the leg at :611, and T8's remarks at :275.

**Classification:**
- `PerRowDeliversEachRowAloneOrCountsItAsOneDrop`: **LAWFUL**, in all three arms. Each assertion follows from a tier-1 text: one row or one drop per call (BC-16 and :575 (3)); a drop of exactly 1, never folded into another call (:575 (3), and L7's "no count"); and nothing after Unsubscribe. A closing call on a per-row subscription is not required: :484 covers batched subscriptions only, and C#'s per-row path is no longer batched (:536 (1)). An empty call, (null, 0), fails the per-call assertion, as BC-16 requires. The fixture self-check uses the public `FletcherCodec.Decode`, so it observes domain 2's own answer.
- `DisposingInsideAPerRowDeliveryIsRefusedAndNothingIsTornDown`: **LAWFUL**. The law requires the subject (L11: "refused with a managed exception"), and it closes d10 Findings (c). One narrowing: `Assert.IsType<InvalidOperationException>` pins the exception type that SA:312 happens to throw. L11 asks only for "a managed exception". This is acceptable for a guard, but no law requires that type.
- `APerRowDictionaryColumnArrivesAsItsValueType`: **LAWFUL**. D32 states the subject outright ("the value type per row"). This is **not** a restoration of the forbidden batched `ADictionaryColumnArrivesAsItsValueType` (V12, D24): that test asserts the value type on a batch, and :575 (5) explicitly allows the per-row and batched types to differ. What it proves is domain 2's per-row answer reaching the handler through C#. It cannot see "C# converts nothing" directly, so it is not the proof of that clause.
- `APerRowHandlerThatUnsubscribesItselfStopsCleanly` (leg): **LAWFUL as a leg**. The reflection only reads the private `_subscriber` field and two internal counters. It never calls or mutates anything, and the ground rules allow legs to observe internals. It is never the sole proof:
  - The rung's promises are proven from outside by the forcing test and the guards.
  - The leg's own external half (calls = 1, no failure, `IsLive` false, absorbed 0, a clean Dispose) stands without the counters.
  - The counters only bracket a mechanism (where the codec is released, DEBT-1/DEBT-2), not a law.
  - The leg now names the right branch (delivery, deferred = 0), as DEBT-1 asked. The design's "Forcing-test mapping" still says "deferred free". That is stale tier-3 text, for the PM to correct.
- T8's remarks: **LAWFUL**. Citing L2 for "carry only a reason the C surface publishes" is a fair reading of "it authors none of it".

**UNMOORED (report only, not blocking):**
- The forcing test asserts `RowLimit` on every call, **drops included**. No ruling sets the per-row reason; the ladder gives it to the architect. L2 narrows the choice to {1, 2, 3}, but "only RowLimit is true" is the design's argument, not a law. For a drop, RowLimit is not d7's "max_rows good rows reached", because d7 counts only good rows. SA:118 must define it per row, as the design review asked.

**Laws against the corpus (gaps, not violations):**
- Arm `unopenable` never checks that `DictionaryOfStructs` is refused by the codec. If the codec opened that schema, the TwoColumns-encoded rows would still drop, so the arm could pass for the wrong reason. A one-line fixture check would close this.
- `subscribedFirst` cannot tell a schema-less latch (V4, forbidden by :258 (3) and :551 (4)) from a schema that never arrives. It does not resist removing the latch, so it is not tolerance. But no per-row test proves the brief's visible change "a first row with no layout no longer disables the subscription". That clause has no per-row test until a carrying-transport arm exists.
