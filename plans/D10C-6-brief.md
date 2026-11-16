# D10C-6 — Stage Brief (2026-11-16)

**In one sentence:** one Arrow subscriber object refuses to hold a per-row and a batched subscription to the same topic, in either order, so the later can never silently replace the earlier once both share one native subscriber (rung 7).
**Forcing test:** `SubscriberArrowTests.ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond` proves that the second kind is refused by name, that the first keeps delivering, and that the other kind is accepted once the first is gone.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Per-row and batched subscribe on the Arrow subscriber | CHANGED: throw `InvalidOperationException` naming the topic and the kind already held, before anything is subscribed | Ruling :575 (6), d10 L3 "ruled refusal", D33. Revises :448 (1) "documented, not refused" for C# only; C++ is unchanged |
| Unsubscribe / disposing a subscription | CHANGED: once the close returns, the topic is free for either kind | Ladder rung 6: "accepted once the first is gone" |

No new public type or member. d10 asks for "a named exception" but names no type. I chose `InvalidOperationException` (rather than `FletcherException`) for two reasons:
- No lower layer answers this question (d9 D33: the transport replaces silently), so there is no status or origin to forward. A `FletcherException` would claim a C status and origin that never happened (L2).
- The binding's other refusals of its own state already use `InvalidOperationException`: the Arrow subscriber's `Dispose` inside a delivery, and `Subscriber`'s re-entrant subscribe.

## Deleted
Nothing. The stage adds one refusal the ruling requires. No code, test or document asserts the mix (0a audit CLEAR), and `TopicOptionsReachTheProviderThroughBothForms` uses two different topics, so it stays green unchanged.

## Corner cases forbidden vs handled
**Forbidden:**
- **The mix on one object, in either order:** refused before any batcher, native call or handler exists, so nothing is left to free.
- **Two threads racing the two kinds on one topic:** the claim is taken under the object's lock before the subscribe call, so exactly one wins.
- **A refused or failed subscribe leaving a stale claim:** every throw after the claim releases it.
- **A second unsubscribe releasing twice:** the claim is keyed by the subscription object, so a second release is a no-op.
- **An unsubscribe that fails:** it keeps the claim, so the object refuses rather than risk a silent replacement; a retry releases it.
- **Subscribing after the object is disposed:** this already throws "disposed" before the check runs.

**Not refused (L3: no rule without a ruling):**
- the same kind twice on one topic;
- a schema watch on a held topic;
- different topics;
- two Arrow subscribers on one provider. The ruling's scope is decided by L3's own text: "this object's own rule … the transport's rule between separate subscribers stays domain 3/4's".

**Handled:** none.

## How
- **Mechanism, all in `dotnet/src/Fletcher/SubscriberArrow.cs`:**
  - Two maps under the existing `_gate`: topic → (kind, count) and subscription → topic.
  - `Subscribe` and `SubscribeBatched` claim the topic after their argument checks and before the managed `Batcher` is built or the inner subscribe runs. They release the claim on any throw and record the subscription on success.
  - `Unsubscribe` releases after the inner unsubscribe returns normally. `Subscription.Dispose` already routes there through `Canceller`.
  - `Dispose` is untouched.
- **Survives rung 7:** the claim, record and release live at the object's public entry points, not in `Batcher` or `RowDelivery`. Rung 7 replaces only the calls between them.
- **Size:** about +45 / −0 production lines (comments and exception docs included) in 1 file. This is inside light-lane admission (≤60 touched lines, ≤3 files). If it passes 60, I return EJECT.
- **Tests (not counted):** about +100 lines in `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`.
  - A theory over both orders. After the refusal, a publish reaches the first handler (batched uses `MaxRows = 1`), and the refused handler is never called (one-sided wait).
  - It then releases the first subscription, by `Unsubscribe` in one order and `Subscription.Dispose` in the other, and the other kind is accepted. This arm is the control: a claim that is never released fails it.
  - Guards, green both ways: `TheSameKindTwiceAndDifferentTopicsAreNotRefused` and `TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther`.
- **Red for the right reason:** today the second call returns a subscription in both orders ("No exception was thrown").
- **Verify, from `dotnet/`:**
  - Build: `dotnet build Fletcher.slnx -c Release -m:1 -p:FletcherNativeShim=<edge> -p:FletcherProbeShim=<probe> -p:FletcherServerShim=<server>`.
  - Edge leg: `dotnet test Fletcher.slnx -c Release --no-build -m:1`.
  - Server leg: copy the server file into `tests/Fletcher.Tests/bin/Release/{net8.0,net10.0}`, run `dotnet test tests/Fletcher.Tests -c Release --no-build`, then remove it. This follows the D10C-5 evidence row.
- **Docs (not counted):** d10's L3 mark changes to built for the same-topic refusal, and the §4 bullet at :447-449 is updated.

## Decisions for you
None. The stop checks are clear:
- **Scope** (one object) is settled by L3's text.
- **No new public surface:** the refusal uses an existing .NET exception type.
- **Ruled exception:** the check answers no lower layer's question. d9 D33 leaves the mix unrefused, so this is L3's ruled exception, not a duplicate check.
- **No d7 or d9 edit, and no test is skipped.**

## Risks accepted / debt carried
- **Rung 7 must re-anchor this test's batched-first arms for the edge leg.** There, batched subscribe throws "not supported", so that arm becomes: the refused batched call leaves no claim, and a per-row subscribe to the same topic is accepted. The ladder's rung-7 re-anchor list does not include this test yet. PM: please add it.
- **Rung 7 must keep releasing the claim only after the native close returns.** If an in-handler close returns before the transport subscription is gone (d7 L9), rung 7 holds the claim until it is gone; that is rung 7's design, not this stage's.

## Numbers
Declared net lines: production about +45 / −0 in 1 file; tests about +100 in 1 file. New public surface: 0. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did
not predict, fix cycles used>.
