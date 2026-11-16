# D10C-6 — step 0a test-authority audit (corpus)

**Verdict: CLEAR.** No in-scope test asserts, or exercises and accepts, a row and a batched subscription to the same topic on one `SubscriberArrow`.

**Proof of read:** `plans/RIX-rulings.md`, 649 lines (wc: 648, no trailing newline), 50 `## ` entries, read top to bottom. Last heading: :643 "## 2026-11-16 — D10C-5: the stage-size overrun". d10 §1 and §3 L3 (:149-181) read, including "Ruled refusal: the same-topic mix (ledger :575 (6), D33)", `[designed-not-built]`.

**Yardstick:** ruling :575 (6), at :589-590: "SubscriberArrow refuses a row and a batched subscription to the same topic (a ruled exception under d10 L3; revises :448 (1)'s 'documented, not refused' for C#)". d10 L3 says the rule is "this object's own rule ... the transport's rule between separate subscribers stays domain 3/4's". :448 (1) still holds for C++/C (d9 D33). Not re-audited here.

**Scope covered (HEAD 54b9ee2):**
- `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`: every `[Fact]`/`[Theory]`, the fixture (one `SubscriberArrow` per test) and the reflection helper at :622. No test is skipped. Only one test uses both kinds: `TopicOptionsReachTheProviderThroughBothForms` (:258), on the distinct topics `optrow`/`optbatch`, and both calls are refused NotSupported. **LAWFUL.** Rung 6 leaves it green. `PerRowDeliversEachRowAloneOrCountsItAsOneDrop`'s "mixed" arm (:406) mixes row kinds, not subscription kinds. **LAWFUL.**
- `dotnet/tests/Fletcher.Tests/ReentrancyTests.cs`: its fixture is a raw `Subscriber` (:44, :53), and it never uses `SubscriberArrow` or `SubscribeBatched`. Its subject lies outside :575 (6). **LAWFUL.** `SubscribingToATopicThisSubscriberAlreadyHoldsIsServedFromInsideAHandler` (:226) subscribes the same topic twice with the same kind (per-row), on a raw `Subscriber`.
- `integration-tests/pubsub-transport-conformance/.../TopicOptionsOverFastDdsTests.cs`: `ASubscriberArrowForwardsItsReaderProfileOnBothForms` (:352) runs the two forms in sequence, on the different `Unique("arrowreader")` and `Unique("arrowreaderbatched")` topics, and disposes the first subscription before the second. Its refusal arms (:385, :387) also use distinct topics. **Answer to the brief: no, it does not use both forms on the same topic.** **LAWFUL.**
- `integration-tests/preview-bundle-consumer/.../Program.cs`: the per-row (:120, a generated subscriber) and batched (:140) subscriptions are on separate providers in separate functions. **LAWFUL.** `Fletcher.TestHost/Program.cs` is per-row only. **LAWFUL.**
- Gateway lanes (`BatchedSubscriptionTests.cs`, `EndToEndTests.cs`) use domain 5's `FletcherClient`, not `SubscriberArrow`. Out of subject.

**Scheduled restoration:** none. No ladder row, manifest or comment plans a test of the same-topic mix. Ladder :83 orders the refusal (rung 6) before the shared `fl_subscriber` (rung 7).
**Laws without tests:** the L3 same-topic clause is `[designed-not-built]` with no test, as expected. Rung 6's forcing test supplies it. No false green.
