# D10C-7 — step 0a test-authority audit (corpus), rung 7 (the switch)

**Verdict: MUST-DELETE(5)** — plus one restoration hazard to strike (R1) and four corpus items the ladder missed (M1-M4).
HEAD `79b9d3a`. **Proof of read:** `plans/RIX-rulings.md` read top to bottom: **648 lines, 50 entries**; last heading
"## 2026-11-16 — D10C-5: the stage-size overrun" (:643). Yardstick: d10 §1, §3 L3, L4, L6, L8-L10, L12-L14, §5 D24-D34, §8 rule 6;
d7 V1-V4, V9-V13 (via d10 L14); d9 L12, L14; register BC-8, -9, -12, -13, -14; rulings :140, :203, :484, :500, :551, :575, :595.
No tier-1 disagreement found. No `Skip`/`[Ignore]`/`#if false` in `dotnet/tests` or `integration-tests`.

## Scope enumerated (file `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` = SAT unless named)
30 SAT tests touch `SubscribeBatched` (ladder's 26 + T31 + the 3 D10C-6 tests); helpers `Options`/`Sink`/`SubscribeKind` (SAT:134, :365, :1626);
TopicOptionsOverFastDdsTests:352; preview-bundle-consumer `NativeBatch` (Program.cs:129); T40 (SAT:1582, rung 8); T45.
Ladder lists verified against the tree: all 5 deletes and 21 re-anchors exist and call `SubscribeBatched`. **No count drift.** The
"batched twins of T32, T41" do not exist: T32 (:1292) and T41 (:1595) now call per-row `Subscribe`; the twins are NEW tests, not re-anchors.

## MUST-DELETE (subject forbidden; delete, never re-anchor). Nothing schedules any return.
| T | SAT line, test | Forbidding text |
|---|---|---|
| T20 | :938 `ADictionaryColumnArrivesAsItsValueType` | :551 (2) "A batched dictionary column reaches C# re-folded, as domain 7 delivers it"; d10 D24, V12 |
| T30 | :1251 `ATimeoutPastTheTimersMaximumIsRefused` | :536 (3) "An out-of-range batch option raises domain 7's refusal ... the 49.7-day cap goes"; :575 (4) FletcherException, not `ArgumentOutOfRange` |
| T34 | :1354 `ATimeoutFlushAndARowLimitFlushMayOverlap` | d10 L13 "Batches reach a C# handler one at a time, in window order"; :551 (4) revises D-BIND-64 |
| T38 | :1536 `ARowThatWouldPassTheCeilingFlushesTheWindowFirst` | d10 L4 "The binding holds no window, row count, deadline, timer or batch thread of its own" (V9). Port precondition (:203) met: d7 heavy `ARowThatMeetsTheBudgetClosesTheWindowAndOpensTheNext` (D10C-1) |
| T39 | :1563 `ARowLargerThanTheCeilingIsDroppedAndCounted` | d10 L4 (as T38); case held by d7 heavy `CraftedRowExceedsTheOffsetBudgetEvenAlone` |
T40 (:1582) is FORBIDDEN by the same L4 sentence; deletion stays at rung 8 as scheduled (it pins the constant rung 8 deletes). Must not be re-anchored.

## RE-ANCHOR (lawful subject). LEGITIMATE-BUT-ADJACENT unless marked.
- **Plain re-anchor onto the server leg + edge truth table:** T12-T19, T22-T24, T26-T28, T33, T35, T37. **Read-straight-after-Publish rewrites**
  (:140's precedent, "must wait for delivery instead"; L13 batch thread): T12 :788, T14 :828, T17 :883, T22 :1029, T23 :1059, T27 :1186.
  T26 (:1127): the handler also receives the Closing (:484) and re-calls `Unsubscribe` on a dead subscription; ignore the Closing call.
- **Forbidden assertion, lawful subject (rewrite the assertion, keep the test):** T21 :972 and T36 :1477 cast the dictionary column to
  `StringArray` (D24: assert `DictionaryArray`, nulls/values kept); T25 :1116 `delivered == 1` excludes the closing delivery
  (:484 "every close ... ends with exactly one closing delivery"): expect one empty `Closing`, then nothing.
- **FORBIDDEN-BY-TOLERANCE → re-anchor:** T7 :259: both forms assert only `NotSupported`, which the edge shim answers for the build. Server
  leg: assert the provider's refusal, distinguishable from the edge text (message); edge leg: the truth table.
- **D10C-6, LAWFUL** (d10 L3 "refuses a row subscription and a batched subscription to the same topic ... whichever comes second"):
  `ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond` :383 (server leg as written; edge leg per FWD-1/-1b:
  batched-first arm asserts the truth table, no claim left); guards `TheSameKindTwiceAndDifferentTopicsAreNotRefused` :425 (batched arms
  server-only; edge: truth table) and `TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther` :449 (both arms server-only; lawful per L3
  "the transport's rule between separate subscribers stays domain 3/4's": it must not gain a delivery assertion that accepts d9 (b2)'s
  silent replacement). No `InvalidOperationException`/`NotSupported` confusion: the types differ.
- **Keep:** T45 `ThunkDisciplineTests.*` (the batch thunk joins its scope, L10).

## R1 — restoration hazard (blocking until struck; not a tier-1 conflict)
d10 L3 Tests (`d10:181-182`): T30 "pins the BC-8 copy (**to retire or re-anchor**; §8)". §8 rule 6 (the principle, which outranks the clause)
and the ladder say delete. Strike "or re-anchor" when rung 7 lands; d10 L14 V12 (`d10:378`) also still cites T20 as the evidence of today.

## Missed by the ladder (M)
- **M1 T31** `AWindowAtTheMaximumTimeoutWaits` (SAT:1270) calls `SubscribeBatched`; ladder re-anchors it only at rung 10. From rung 7 its
  edge leg gets `NotSupported`: re-anchor its leg handling at 7 (subject lawful; `MaxTimeout` itself goes at 10).
- **M2 TopicOptionsOverFastDdsTests.ASubscriberArrowForwardsItsReaderProfileOnBothForms** (:352, batched half :367, :387): the
  pubsub-transport-conformance lane stages only the edge shim (`ci.integration-test.pubsub-transport-conformance.yml:86-101, :169-197`).
  Re-anchoring its batched half to the edge truth table would prove nothing about options (T7's tolerance, moved): it needs a server-shim run.
- **M3 T46** preview-bundle-consumer `NativeBatch` references only base packages (`PreviewConsumer.csproj:28-30`): red from rung 7, re-anchored
  only at 12. Undeclared red window: declare it, or reference `Eiva.Fletcher.Server` (exists since rung 5) at 7.
- **M4** the three D10C-6 tests are absent from the rung-7 row; they are in scope by FWD-1/-1b/-1c (design-debt.md:903-910).

## Laws vs build marks
No false green. L6, L8, L13, L14 are `[designed-not-built]` with no test (rung 7 creates them); L3's edge sentence (D29) likewise;
L4's structural scan is rung 8's. Every `[built]` law in scope has a named test.
