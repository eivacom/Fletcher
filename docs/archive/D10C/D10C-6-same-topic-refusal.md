# D10C-6 — same-topic refusal: design (as briefed, and as implemented)

*Written after the stage ejected from the light lane: the first build touched +113 / −12 production
lines against an admission of 60 and a brief of about +45. This records the brief's design, the code as
it stands after fix cycle 1 (uncommitted, diff base `928bb7d`, +103 / −2), and how the two differ. The
reviewers ruled on the size (§Size).*

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
1. **Subscribe** (`SA:222`) and **SubscribeBatched** (`SA:247`) run their existing checks first
   (disposed, null handler; for batched the managed options check, which rung 10 removes), then build
   the `RowDelivery` or `Batcher`. Both then call one helper, `SubscribeClaimed` (`SA:289`).
2. The helper refuses a defaulted topic (`ToKey()`), then `Claim` (`SA:317`) under the lock: a free
   topic is taken, the same kind increments the count, the other kind throws.
3. Only the inner subscribe sits in `try { … } catch { Release(topic); throw; }`. The id is recorded
   right after the `catch`, outside it, and the id, canceller and batcher wiring follow in the caller.
4. **Unsubscribe** (`SA:366`): `BelongsTo` refuses a foreign subscription first, as before. It
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
| Topic key | `TopicPath` | `TopicPath` (first build: `ToString()`, changed by DEBT-1) | A default topic is refused before the claim (N2), so `<default>` is never a key. |
| Release on failure | every throw after the claim | `try/catch` around the inner subscribe only (DEBT-1; first build: the whole tail) | Closer than the brief's words: the claim lasts exactly as long as the native subscription, because `Record` runs after the `catch` (F1/N1). |
| Claim, record, release | inline in each entry point | one helper, `SubscribeClaimed` (S1) | Rung 7 changes one place. |
| Count, release after close | as brief | as brief | Not a deviation. |

**Residue, not widened:** if a line after the helper returns (the id, canceller or `_batchers` wiring)
throws, the native subscription is not undone, as before this stage. Only out-of-memory can throw there,
and the claim is now kept while that subscription lives, so the other kind stays refused.

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
- Ledger: `plans/RIX-rulings.md`, 648 lines, 50 entries, last :643 (D10C-5 overrun); unchanged.

## Forcing-test mapping
All in `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs:357-457`.
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
**The first build, +113 / −12:** exception docs 8; fields 4; two `try/catch` wrappers about 14 plus about
12 re-indented lines; `Claim` 25; `Release` 17; `Record` 7; `Unsubscribe` 12. The brief's +45 under-counted
the docs and the re-indentation that wrapping the whole tail forces; the light-lane call was mine, wrong.

**The smaller mechanism, taken in fix cycle 1: +103 / −2.** The `try` covers only the inner subscribe
(nothing re-indented); claim, record and release live in one helper; the key is `TopicPath`. The saving
is small (about 10 lines) because the subject itself is about 70 lines: a claim, a failure release, a
close release, a per-subscription record and docs (about 15 lines of comments, 8 of them exception
docs). The stage would not have fitted the light lane even written tightly. The behavioural gain is the
claim lasting as long as the native subscription.

## Files-to-touch
- `dotnet/src/Fletcher/SubscriberArrow.cs` (production; done, uncommitted).
- `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs` (one theory and two guards; done).
- `docs/domains/d10-csharp-binding.md`: the L3 mark (`[designed-not-built … the same-topic refusal]`
  becomes built), L3's Tests line, and the §4 bullet at :447-449.

## Files-to-delete
None. This stage adds a ruled refusal. No construct asserts or carries the mix: the 0a audit is CLEAR,
and the C++ "documented, not refused" (d9 D33) stays for C++ by ruling :575 (6).
