# D10C-6 — step 0b test-authority audit (new-test)

**Verdict: CLEAR.** No new test asserts, or exercises and accepts, behaviour a tier-1 authority forbids.

**Proof of read:** `plans/RIX-rulings.md`, 649 lines (wc: 648, no trailing newline), 50 `## ` entries, read top to bottom; last heading :643 "## 2026-11-16 — D10C-5: the stage-size overrun". d10 L3 (:149-181) read.

**Yardstick:** :589-590 (6) "SubscriberArrow refuses a row and a batched subscription to the same topic (a ruled exception under d10 L3; revises :448 (1)'s 'documented, not refused' for C#)"; d10 L3 "The binding checks a rule itself only where a ruling accepts the duplication" and "It is this object's own rule … the transport's rule between separate subscribers stays domain 3/4's".

**Scope (diff vs 928bb7d, `SubscriberArrowTests.cs` +99), all on the public API only (ctor, `Subscribe`, `SubscribeBatched`, `Unsubscribe`, `Subscription.Dispose`; helper `SubscribeKind` only routes):**
- `ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond` (theory, both orders): **LAWFUL**, asserts :575 (6) directly. Red for the right reason (no throw today). The message check is adequate. It needs the topic and the kind already *held*, which is the opposite of the call being refused, so a generic failure of that call does not match. The delivery, one-sided absence and release-then-accept arms carry the weight on their own.
- `TheSameKindTwiceAndDifferentTopicsAreNotRefused`: **LAWFUL**. It guards against an unruled refusal (L3 principle). Same kind twice shares one provider subscription per topic in `Subscriber` (C# and C++), so it does not touch the d9 (b2) collision. It asserts no delivery and pins no transport semantics.
- `TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther` (theory): **LAWFUL**. Its subject is what L3 *requires*: the binding adds no cross-object rule, because no ruling licenses one. The transport behaviour it relies on is the separate-instance case: seam §7 clause 4 is "one callback per topic *per instance*". That is not the within-instance collision registered for domains 3/4. It asserts no-throw only, so it pins no transport delivery semantics.

**New VALUE subject:** none. Each arm is in ladder rung 6, the brief's "Not refused" list, or L3's text.
**Scheduled restoration:** none.
**Laws without tests:** L3's same-topic clause is supplied by this test; no false green.

**Non-blocking note (rung 7 survival; for the PM's debt register):** FWD-1 re-anchors only the *batched-first* arms. On the edge leg at rung 7, `SubscribeBatched` throws NotSupported, which breaks more than those arms:
- the *row-first* arm. Its control re-subscribes batched. Its refusal also expects `InvalidOperationException` only if the claim check runs before the NotSupported check.
- both batched arms of `TheSameKind…`.
- both arms of `TwoArrowSubscribers…`.

FWD-1 should name all three tests for the edge leg. The server leg survives as written.
