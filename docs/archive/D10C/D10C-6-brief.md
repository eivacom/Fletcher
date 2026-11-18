# D10C-6 — Stage Brief (2026-11-16)

**In one sentence:** one Arrow subscriber object refuses to hold a per-row and a batched subscription to the same topic, in either order, so the later can never silently replace the earlier once both share one native subscriber (rung 7).
**Forcing test:** `SubscriberArrowTests.ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond` proves that the second kind is refused by name, that the first keeps delivering, and that the other kind is accepted once the first is gone.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Per-row and batched subscribe on the Arrow subscriber | CHANGED: throw `InvalidOperationException` naming the topic and the kind already held, before anything is subscribed | Ruling :575 (6), d10 L3 "ruled refusal", D33. Revises :448 (1) "documented, not refused" for C# only; C++ is unchanged |
| Unsubscribe / disposing a subscription | CHANGED: once the close returns, the topic is free for either kind | Ladder rung 6: "accepted once the first is gone" |

No new public type or member. d10 asks for "a named exception" but names no type, so I chose `InvalidOperationException`:
- No lower layer answers this (d9 D33), so there is no C status or origin to forward (L2).
- The binding's other refusals of its own state already use it.

## Deleted
Nothing. The stage adds one refusal the ruling requires. No code, test or document asserts the mix (0a audit CLEAR), and `TopicOptionsReachTheProviderThroughBothForms` uses two different topics, so it stays green unchanged.

## Corner cases forbidden vs handled
**Forbidden:**
- **The mix on one object, in either order:** refused before any batcher, native call or handler exists.
- **Two threads racing the two kinds on one topic:** the claim is taken under the object's lock before the subscribe call, so exactly one wins.
- **A refused or failed subscribe leaving a stale claim:** every throw after the claim releases it.
- **A double release:** the release is keyed by subscription, so a second one is a no-op.
- **An unsubscribe that fails:** it keeps the claim (refuse rather than risk a silent replacement); a retry releases it.
- **Subscribing after the object is disposed:** this already throws "disposed" first.

**Not refused (L3: no rule without a ruling):** the same kind twice; a schema watch; different topics; two Arrow subscribers on one provider. L3's own text settles the scope: "this object's own rule … the transport's rule between separate subscribers stays domain 3/4's".

**Handled:** none.

## How
- **Superseded by the design doc** `plans/D10C-6-same-topic-refusal.md`, written after the ejection. It covers the mechanism as briefed and as implemented, the deviations, the pre-existing failure tail, and whether a smaller mechanism exists.
- **Tests (not counted)**, in `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`:
  - the forcing theory over both orders, whose accept-after-release arm is the control;
  - guards `TheSameKindTwiceAndDifferentTopicsAreNotRefused` and `TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther`.
- **Red for the right reason:** before the change, the second call returned a subscription in both orders ("No exception was thrown").
- **Verify, from `dotnet/`:**
  - Build: `dotnet build Fletcher.slnx -c Release -m:1 -p:FletcherNativeShim=<edge> -p:FletcherProbeShim=<probe> -p:FletcherServerShim=<server>`.
  - Edge leg: `dotnet test Fletcher.slnx -c Release --no-build -m:1`.
  - Server leg: copy the server file into `tests/Fletcher.Tests/bin/Release/{net8.0,net10.0}`, run `dotnet test tests/Fletcher.Tests -c Release --no-build`, then remove it. This follows the D10C-5 evidence row.

## Decisions for you
None. The stop checks are clear:
- **Scope** (one object) is settled by L3's text.
- **No new public surface:** the refusal uses an existing .NET exception type.
- **Ruled exception:** the check answers no lower layer's question. d9 D33 leaves the mix unrefused, so this is L3's ruled exception, not a duplicate check.
- **No d7 or d9 edit, and no test is skipped.**

## Risks accepted / debt carried
- **Rung 7 must re-anchor this stage's tests on the edge leg**, where batched subscribe throws "not supported" (FWD-1, FWD-1b in `plans/reviews/design-debt.md`).
- **Rung 7 must release the claim only after the native close returns.** If an in-handler close returns before the transport subscription is gone, the claim is held until it is.

## Numbers
**Ejected from the light lane.** Production lines: declared about +45 / −0 in 1 file; **actual +113 / −12 (125 touched) in 1 file**, against an admission of 60. Now in the full loop. Tests: about +100 in 1 file. New public surface: 0. Design cycles used: 1/2.

---
**As landed (2026-11-16):**
Ejected from the light lane (first build 125 touched vs 60); full loop run. Final prod +103/−2 in 1 file (one `SubscribeClaimed` helper, narrow try, TopicPath key). Tests +102. Design cycles 1, fix cycles 1, implementer passes 4. FWD-1/1b/1c carry obligations to rung 7.
