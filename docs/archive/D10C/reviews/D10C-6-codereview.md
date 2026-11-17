# D10C-6 code review (step 4b): same-topic refusal

Diff base `928bb7d`, working tree: `SubscriberArrow.cs` (+104/−2), `SubscriberArrowTests.cs` (+101).
Re-run here: `SubscriberArrowTests` 52 of 52 on net10.0 (edge shim `fletc587b627541ee4`).
**Verdict: nothing blocking; 1 should-fix, 4 nits.**

## Checked, clean
- **Lock discipline and reentrancy:** the outer `_gate` is held only around dictionary operations. It is never held across the inner subscribe or close, `Batcher.Stop`, or a handler, and `Batcher` and `RowDelivery` lock their own gates. An in-handler subscribe of the other kind is refused by `Claim` before the inner in-delivery check. An in-handler self-`Unsubscribe` releases the claim when the carve-out returns (P1). I found no lock-order cycle.
- **Claim count, every path:** a refusal takes nothing. If the inner subscribe throws (disposed, in-delivery, default topic, native status), the claim is released. On close, `_claimed.Remove` returns true exactly once, so a double or concurrent `Unsubscribe` or `Dispose` releases once. If the close fails, the claim is kept and a retry releases it, because the inner close of a retired id is a no-op that returns OK.
- **Dispose:** `ObjectDisposedException` fires before `Claim`. A `Subscription.Dispose` after the arrow is disposed routes to the inner no-op and then releases harmlessly. Leaving `_claims` uncleared is inert.
- **"Same topic":** keyed by `TopicPath`, whose equality is ordinal on the joined key. It is case-sensitive and does no Unicode normalisation, which is exactly the wire's byte identity. Separate `Of(...)` instances with equal segments are one key. `'/'` cannot appear inside a segment, so the join is injective.
- **Forcing test not vacuous:** `Assert.Throws` pins both the topic and the held-kind phrase. The red-first runs are recorded in the evidence, and the accept-after-release arm fails if the claim leaks. The guards' assertion is "no throw", which is right for them.
- **Flakiness:** low. Absence is checked with a one-sided 300 ms wait, which can only pass falsely. Presence waits up to 10 s on the in-process provider, and every test uses its own topic names.
- **Comment bulk:** about 15 of the 104 production lines are comments (8 of them exception docs). The overrun is structural, not prose; see S1.

## Findings
**S1 should-fix (medium confidence): fold the duplicated claim block into one helper.** Both entry points repeat `Claim; try { inner; Record } catch { Release; throw }`, about 12 lines each. One helper, `SubscribeClaimed(topic, perRow, Func<SubscribeResult>)`, would hold claim, record and release in a single place. That saves about 12 lines and gives rung 7 one site to re-anchor instead of two. `Record` (7 lines) then folds into it.

**Nits**
- N1: `Record` sits inside the `try`, so a throw there releases the claim while the native subscription lives. Only out-of-memory can do this. Recording after the `try` keeps the claim exactly as long as the subscription (the brief's own narrowing).
- N2: a default `TopicPath` is claimed before the inner `ToKey` refuses it. Refuse `!topic.IsConstructed` at the door instead; then a concurrent pair of defaults cannot get the mix message.
- N3: `Release` tolerates a missing claim through `TryGetValue`, which no path reaches (every `Release` pairs with a `Claim`). Index the dictionary directly.
- N4: the accept arm checks only `refused.WaitFor(1)`. Asserting the value 6 pins that the row came from the accepted subscription.

## RECORD
- `plans/D10C-6-same-topic-refusal.md` §"As implemented", §Design step 3 and §Size describe a superseded tree: key `topic.ToString()`, a `try` over the whole tail including the `RowDelivery`/`Batcher` construction, +113/−12, and the `SA:` line anchors. The tree keys by `TopicPath`, builds both before the claim, wraps only the inner subscribe and `Record`, and stands at +104/−2.
