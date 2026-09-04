# PDA-DEC-A4 — independent code review (correctness / safety)

Diff base `963bde5` -> `aa72813`. Scope: correctness, concurrency safety, reachable-and-silent
edge cases, and simplifications in the forbidding direction. Design conformance is a different
reviewer's remit and is not addressed here.

**Counts: 1 blocking, 3 should-fix, 7 nits.**

## Did the guarantee itself break?

**No — and I tried hard.** I built the real `pubsub/src/subscriber.cpp` against a stub provider that
honours "one callback at a time" per subscription and ran a churn harness: 4 topics, 4 delivery
threads, 4 subscribe/unsubscribe threads, 40 rounds, each subscription carrying heap state that the
canceller marks "freed" the instant `Unsubscribe` returns.

- HEAD (`aa72813`): **385,745,526 callback invocations — 0 running-at-return, 0 post-return.**
- Base (`963bde5`), same harness as a non-vacuity control: **7,793 running-at-return, 2,496
  post-return invocations.**

So the harness detects the defect and the mechanism removes it. Analytically the core interleaving is
airtight and stronger than the design claims: because the reader takes the gate *before* loading
`retired`, and the retirer releases the gate before returning, the mutex — not the provider's
"one at a time" contract — carries the guarantee. Even a provider that illegally fans one
subscription out on two threads is handled (one delivery is waited for, the other reads `retired`
under the same mutex). Memory ordering is sound: the acquire load reads the release store through a
mutex synchronises-with edge, so `true` is guaranteed rather than probabilistic. The thread-local
token stack is RAII-unwound on every exit including a throwing callback, and the token cannot be
recycled while it is on the stack, because the executing `std::function` copy pins the `Identity`.

The one thing that *is* broken by the diff is on the neighbouring path, and it is blocking.

---

## BLOCKING

### B1 — the drain widens the provider-transition window from instructions to the whole handler, and a `Subscribe` landing in it is silently killed (confidence: high, reproduced; regression vs. base)

`Subscriber::Unsubscribe` publishes `provider_subscribed = false` under `mu`, releases `mu`, then
calls `RetireAndDrain` — which now blocks for **the entire duration of an in-flight handler**, an
explicitly unbounded time — and only then calls `provider->Unsubscribe(segments)`.

A `Subscribe` on the same topic that lands in that window takes `mu`, sees `provider_subscribed ==
false`, creates a **fresh provider-level subscription**, and sets `provider_subscribed = true`. The
first thread then finishes draining and tears that brand-new subscription down. Nothing repairs it:
`provider_subscribed` is left `true`, so no later `Subscribe` re-registers either. The newcomer's
`Subscribe` returned success, holds a valid id, and **receives nothing, ever, with no error
anywhere**.

Reproduced (`C:\tmp\repro_pda.cpp`, 600 ms handler, newcomer subscribing 150 ms into the drain):

- HEAD: `provider subs=2 unsubs=1  live provider cbs=0  late_calls=0` -> newcomer dead.
- Base: `provider subs=2 unsubs=1  live provider cbs=1  late_calls=1` -> newcomer receives.

The race shape predates the diff, but before the diff the window was a few instructions and this same
test passes; the drain turns it into a routinely-hit window. Reachable in shipping code:
`gateway/src/gateway.cpp:62` holds **one `shared_ptr<Subscriber>` shared by every WS session**, and
`ws_session.cpp:246,334` unsubscribe from session threads while other sessions subscribe.
`~Subscriber` has the same shape (drain, then `provider->Unsubscribe(segs)`) against any second
`Subscriber` sharing the provider.

**Fix (one line of intent):** never leave the provider transition half-committed across the drain —
retire+drain first, then re-take `mu` and only clear `provider_subscribed` / enter
`provider->Unsubscribe` if the topic is *still* empty, under a per-`TopicState` transition mutex
(order `mu` -> transition -> provider) that `EnsureProviderSubscription` also holds across its
`provider->Subscribe`.

---

## SHOULD-FIX

### S1 — the published lock order is the reverse of the enforced one, and the published one deadlocks (confidence: high)

`subscriber.cpp:112` states "the lock order is total, `mu` < gate < provider". The code enforces the
opposite edge: the fan-out holds `entry.gate->mu` **across the user callback**, and that callback may
re-enter `Subscriber::Subscribe`/`Unsubscribe`, which takes `mu`. So the real edge is **gate -> mu**,
and the suite already has a control for it (`ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock`).
There is no cycle today only because no path takes `mu` and then a gate — but the comment explicitly
licenses exactly that, and `RetireAndDrain` is precisely the call a maintainer would hoist into the
`mu` critical section it is currently (deliberately) kept out of. Doing so deadlocks against any
re-entrant `Subscribe`.

**Fix:** publish the order as **gate < mu** ("never acquire a gate while holding `mu`"), and pin it
with a debug-only assertion in `RetireAndDrain` that `mu` is not held by this thread.

### S2 — the barrier skip is scoped to the whole `Subscriber` when only the frames this thread holds need it; the narrower rule makes the unsafe state unrepresentable and deletes the token machinery (confidence: medium-high, forbidding direction)

`InsideDeliveryOn` keys on a per-`Impl` identity token, so a handler delivering topic A skips the
barrier for a subscription on topic B **whose callback is running on another thread**.
`provider.hpp` serialises "one callback at a time" *per subscription* and states outright that
different subscriptions may arrive on different threads; Fast DDS runs a listener per reader, so this
is the ordinary case, and `CrossCancellingDeliveriesDoNotDeadlock` is exactly that shape. Such an
`Unsubscribe` returns while the target callback is still running. It is disclosed in
`subscriber.hpp`, so it is not silent — but every piece of new machinery here (`Identity`, the
shared_ptr token, the "never dereferenced / outlives `this`" reasoning, the per-`Impl` scope
argument) exists solely to tolerate that state.

Key the thread-local stack on **the gate this thread currently holds** instead of on the `Impl`:
push `entry.gate.get()` in the fan-out loop, and skip only if that exact gate is in this thread's
set. Then self-cancellation and same-thread nested delivery still skip; a sibling nobody is running
is acquired uncontended (same non-blocking behaviour); and a sibling running **on another thread** is
*waited for* — the guarantee holds where it currently silently does not, and `Identity` disappears
entirely.

This is an owner-level trade, not a drive-by: a genuine two-thread cross-cancel then becomes a loud
hang, and `CrossCancellingDeliveriesDoNotDeadlock` would have to be re-ruled. That is the same trade
the owner already took for the cross-`Subscriber` shape ("a loud hang over a silent use-after-free"),
so the current asymmetry looks unintended rather than chosen. Cost: the push/pop moves onto the
per-entry hot path.

### S3 — a throwing callback silently drops the sample for every remaining subscriber, and escapes untranslated into the provider's delivery thread (confidence: high; pre-existing shape, but the diff rewrote this loop)

`entry.callback(...)` in the fan-out has no handler. The gate and the delivery-scope stack unwind
correctly, so no lock is left held and no state is corrupted — but the loop aborts, so every
subscriber after the thrower silently misses that sample, and the exception unwinds into a
provider/transport thread that has no contract for it (the seam's taxonomy says every entry point
translates). Wrap the invocation in a `catch (...)` that continues the fan-out.

---

## Nits (one line each)

- `UnsubscribeWaitsForAnInFlightDelivery` and `DestructorDrainsAnInFlightDelivery` prove the wait
  with a 200 ms `sleep_for`: a stalled main thread makes a *broken* build pass — false green, never
  false red, and they are controls, so acceptable but not sound-by-construction.
- gtest `ASSERT_*` from non-main threads (the parked callbacks in `caller_tier.cpp`) is only
  guaranteed thread-safe where pthreads is available; on Windows a concurrent failure is formally UB,
  and it only bites on an already-red run.
- If `fletcher-pubsub` is ever linked into more than one module, the anonymous-namespace
  `thread_local g_delivery_stack` duplicates per module and the skip silently stops matching across
  the boundary; single-module today.
- Token uniqueness rests on the executing `std::function` copy pinning the `Identity`; a provider that
  destroyed a running callback (already UB) could let a recycled address make `InsideDeliveryOn` true
  for an unrelated `Subscriber`.
- `Subscribe`'s rollback never calls `provider->Unsubscribe(segments)` after a partially-successful
  `provider->Subscribe`, so a provider-level subscription can survive with `provider_subscribed ==
  false` (pre-existing).
- Two concurrent first-`Subscribe`s on one topic both observe `provider_subscribed == false` and both
  call `provider->Subscribe` — duplicate provider subscriptions, same unlocked window as B1
  (pre-existing).
- `~Subscriber` swallows every provider exception (`catch (...)`), so a transport failure during
  teardown is unreportable by construction.

## RECORD (paperwork, non-blocking)

- `pubsub/src/subscriber.cpp:112` states the lock order as `mu < gate < provider`; the enforced order
  is `gate < mu` (see S1).
- `pubsub/src/subscriber.cpp:121` "Never hold a gate across a provider call" is violated by the
  delivery path itself whenever a handler calls `Publisher::Publish` or `Subscriber::Subscribe`; the
  rule as intended constrains `RetireAndDrain`'s callers only.
