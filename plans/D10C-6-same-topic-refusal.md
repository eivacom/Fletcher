# D10C-6 — same-topic refusal: design (as briefed, and as implemented)

*Written after the stage ejected from the light lane: the implementation touched +113 / −12
production lines, against a light-lane admission of 60 and a brief that declared about +45. This
document records the design the brief stated, the code as it now stands in the working tree
(uncommitted, diff base `928bb7d`), and how the two differ. The reviewers rule on the size (§Size).*

## Summary
A `SubscriberArrow` refuses a per-row and a batched subscription to the same topic, in either order.
The refusal is an `InvalidOperationException` naming the topic and the kind already held, raised
before anything is subscribed. The topic becomes free for either kind once the holder's `Unsubscribe`
(or `Subscription.Dispose`) returns. Authority: ruling :575 (6), d10 L3 "Ruled refusal", D33; it
revises :448 (1) for C# only (C++/C keep "documented, not refused", d9 D33). Rung 6 of
`plans/D10C-test-ladder.md`.

## Design
**State:** two maps on the object, both under its existing `_gate`, which is never held across a
subscribe, a close or a handler.
- `_claims`: topic → (kind, count). The count is the number of subscriptions of that kind on the
  topic, so the same kind twice is allowed (L3: no rule without a ruling).
- `_claimed`: subscription id → topic. This is what a release finds.

**Flow:**
1. **Subscribe** (`SA:222-242`) and **SubscribeBatched** (`SA:256-299`) run their existing checks
   first: disposed, null handler, and for batched the managed options check (rung 10 removes it).
2. Then `Claim(topic, perRow)` (`SA:306-330`), under the lock. Claiming a free topic takes it. The
   same kind increments the count. The other kind throws.
3. Everything after the claim is inside `try { … } catch { Release(claim); throw; }`. That covers
   building the `RowDelivery` or `Batcher`, the inner subscribe, `Record(id, claim)`, and wiring the
   id, canceller and batcher.
4. **Unsubscribe** (`SA:367-400`): `BelongsTo` refuses a foreign subscription first, as before. It
   stops the batcher, then calls the inner unsubscribe. **Only after that returns normally** does it
   remove the id from `_claimed` and `Release` the topic. A failed close keeps the claim, and a retry
   releases it (the inner unsubscribe of a retired id is a native no-op that returns OK).
5. **Dispose:** unchanged; every later subscribe throws `ObjectDisposedException` before the claim.

**Exception type:** `InvalidOperationException` (existing; d10 says "a named exception" and names no
type). No lower layer answers this question (d9 D33: the transport silently replaces), so there is
no status or origin to forward. A `FletcherException` would assert a C status and origin that never
occurred (L2). It matches the binding's other refusals of its own state (`SA` `Dispose` in a
delivery, `Subscriber.cs:406`). New public surface: 0.

**Why the check is lawful under L3:** L3's "native re-checks it" governs duplicated checks. Here
nothing native answers, so this is the single site, by ruling. There is no principle-versus-detail
conflict.

**Survives rung 7:** claim, record and release sit at the public entry points, not in `Batcher` or
`RowDelivery`. Rung 7 replaces only the inner subscribe and close calls between them.

## As implemented: deviations from the brief
| Point | Brief | Implementation | Assessment |
|---|---|---|---|
| Release key | the `Subscription` object | the subscription **id** (`ulong`) | Equivalent today: ids are unique per inner `Subscriber`, and `BelongsTo` runs first. Premise P2 covers rung 7. |
| Topic key | `TopicPath` | `topic.ToString()` | Same identity for constructed topics (ordinal on the joined key). A default `TopicPath` maps to `"<default>"`, is claimed briefly and released when the inner subscribe refuses it. Nit: keying by `TopicPath` itself avoids the question. |
| Count per topic | as brief | as brief | Not a deviation. |
| Release on failure | "every throw after the claim releases it" | `try/catch` around the whole tail | Matches the brief's words, but the `try` is wider than needed (see the pre-existing state below). |
| Release after close | as brief | as brief | Not a deviation. |

**Pre-existing state, not widened:** if the inner subscribe succeeds and a later line in the `try`
throws, the subscribe call throws and the native subscription is not undone. That leak predates this
stage: the old code had the same tail with no undo. The lines involved are `Record`, the id and
canceller assignments, and the `_batchers` insert; realistically only out-of-memory can throw there.
This stage adds one consequence: the claim is released while that orphaned native subscription lives,
so in that state the other kind would be accepted on the topic. **Narrowing the `try` to the inner
subscribe call alone** (record immediately after it returns, wire up outside the `try`) would keep
the claim exactly as long as the native subscription. It is also smaller (§Size). The reviewers
decide; it is not a blocker on the forcing test.

## Corner cases forbidden
- **The mix on one object, either order:** refused at the door (rung 2 of the forbid-before-handle
  ladder), before any `Batcher`, `RowDelivery`, native call or handler exists.
- **A race between the two kinds on one topic:** the claim is taken under the lock before the
  subscribe call, so exactly one wins (an ordering rule, rung 1).
- **A refused or failed subscribe leaving a claim:** the claim is released on every throw.
- **A double release:** the release is keyed by id and removed once, so a second `Unsubscribe` or
  `Dispose` is a no-op.
- **A failed close:** it keeps the claim, so the object errs towards refusing rather than silent
  replacement.
- **After `Dispose`:** the existing `ObjectDisposedException` fires first.

**Not refused, by L3:** the same kind twice; a schema watch; different topics; two `SubscriberArrow`s
on one provider (d10 L3: "this object's own rule … between separate subscribers stays domain 3/4's").

**Handled residue:** none (the out-of-memory tail above is pre-existing, not handled).

## Premises and stop conditions
- **P1.** A per-row subscription's transport subscription is gone when the inner `Unsubscribe` returns
  normally, including the in-delivery carve-out (`Subscriber.cs:555-618`: native first, and no new
  invocation begins after it). *If false, stop: the release must wait for that point.*
- **P2.** Subscription ids are unique across both kinds within one object. That holds today (one
  inner counter), and at rung 7 batched ids carry bit 63 (`h:1121-1127`). *If rung 7's ids could
  collide, rung 7 re-keys the release (by the `Subscription` object) before landing.*
- **P3.** At rung 7, an in-handler batched close may return before the transport subscription is gone
  (d7 L9). *If so, rung 7 holds the claim until it is gone* (FWD-1).
- Ledger: `plans/RIX-rulings.md`, 649 lines, 50 entries, last :643 (D10C-5 overrun); unchanged.

## Forcing-test mapping
All in `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs:357-454`.
- **`ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond(rowFirst)`** (forcing):
  green by `Claim`'s refusal and `Unsubscribe`'s release. Red before the change: the second call
  returned a subscription ("No exception was thrown"). Its accept-after-release arm is the control
  (fails if the claim is never released), via `Unsubscribe` in one order, `Subscription.Dispose` in
  the other.
- **`TheSameKindTwiceAndDifferentTopicsAreNotRefused`** (guard, green both ways): fails if the rule is
  widened beyond the mix (L3).
- **`TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther(rowOnThisOneFirst)`** (guard, green both
  ways): fails if the scope is made provider-wide or static.
- **Retired:** no test. `TopicOptionsReachTheProviderThroughBothForms` uses distinct topics and stays
  green (0a audit).

## Risks / Unknowns
- **Rung 7 edge leg (FWD-1, FWD-1b, `plans/reviews/design-debt.md:903-904`):** there,
  `SubscribeBatched` throws `NotSupported`, which breaks the forcing test's batched-first arms, its
  row-first release-then-accept check (and its row-first refusal, if `NotSupported` precedes the
  topic check), and both guards' batched arms. Rung 7 decides that check order and re-anchors all
  three tests; the server leg survives as written.
- **No coexistence window:** the managed batcher's deletion (rung 8) does not touch this mechanism.
  Size: see §Size.

## Size: inherent, or is there a smaller mechanism?
**Where the +113 / −12 goes** (approximate, from the working tree): exception docs 8; fields and
comment 4; the two `try/catch` wrappers about 14, plus about 12 re-indented lines (the −12 and +12);
`Claim` 25 (a 5-line message included); `Release` 17; `Record` 7; `Unsubscribe` 12.

**Inherent to the subject:** a claim taken before the subscribe call, a release on failure, a release
after close, a per-subscription record, and docs. That is roughly 55-70 touched lines. The brief's
+45 under-counted the exception docs and the re-indentation that wrapping requires. The light-lane
call was mine, and it was wrong.

**A smaller mechanism exists, with the same behaviour:** (1) wrap only the inner subscribe call
(`try { result = _subscriber.Subscribe(…); } catch { Release(claim); throw; }`, then `Record`): it
re-indents nothing and ties the claim to the native subscription's lifetime, closing the consequence
above; (2) fold `Record` into its callers or one shared helper; (3) `Release` as one
decrement-or-remove; (4) key by `TopicPath`.

The estimate is about +65-75 / −2. That is still just over 60, so the subject would not have fitted
the light lane even when written tightly. **Recommendation: accept the size or take the narrowing.**
The narrowing is the only change with a behavioural edge, and it is in the "refuse rather than
replace" direction. The reviewers decide.

## Files-to-touch
- `dotnet/src/Fletcher/SubscriberArrow.cs` (production; done, uncommitted).
- `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` (one theory and two guards; done).
- `docs/domains/d10-csharp-binding.md`: the L3 mark (`[designed-not-built … the same-topic refusal]`
  becomes built), L3's Tests line, and the §4 bullet at :447-449.

## Files-to-delete
None. This stage adds a ruled refusal. No construct asserts or carries the mix: the 0a audit is CLEAR,
and the C++ "documented, not refused" (d9 D33) stays for C++ by ruling :575 (6).
