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


---
---

# RE-CHECK — fix cycle 1 (`10ad452` -> `6d74d53`)

Appended, not a rewrite; everything above is the cycle-0 review as written.
Cumulative for the item: `963bde5` -> `6d74d53`.

**Re-check counts: 1 blocking, 1 should-fix, 6 nits. Cycle-0 status: B1 fixed (differently and
better), S1 fixed, S3 fixed, S2 declined and I accept the decline.**

## Adjudication of the B1 rebuttal — they are right, and the fix is better than mine

### 1. Is the deadlock class they describe real?

**Yes, and it is worse than they say — it is reachable today, not only with a hypothetical driver.**

My proposed per-`TopicState` transition mutex `T` would be held by a teardown across
`provider->Unsubscribe`. `provider.hpp` *requires* every provider to let no in-flight delivery
outlive that call, and `FastDDSPubSubProvider::Unsubscribe` implements it (`delete_datareader`
waits). So: teardown holds `T` -> waits for the in-flight delivery -> that delivery holds its gate
and its handler re-enters `Subscribe` -> `Subscribe` wants `T`. Cycle
`T -> provider -> gate -> mu -> T`. `CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock`
already exercises the gate->mu leg, and `UnsubscribeDoesNotHoldAGateWhileEnteringTheProvider`
already exercises the provider-waits-for-delivery leg. Nothing hypothetical is needed.

I could not find an avoidance they missed *for the teardown side*: any scheme that makes `Subscribe`
**wait** for an in-progress `provider->Unsubscribe` closes the same cycle, because the waiting
`Subscribe` may be the delivery the teardown is waiting for. The only shape that would work is an
epoch-and-repair (record a subscribe generation before releasing `mu`; after `provider->Unsubscribe`
returns, re-take `mu`, and if the generation moved, re-subscribe) — more machinery, a transient gap
instead of permanent silence, and its own races. Declining the mutex was correct.

Note for the record: their caveat *"none of the three shipped providers delivers synchronously from
`Subscribe` today; a driver that did would make the cycle reachable"* is attached to the wrong half.
Synchronous delivery from `Subscribe` is what would endanger the **subscribe** side; the teardown
side is endangered by `Unsubscribe` waiting for a delivery, which is mandatory and already
implemented. The decision is right; the reason recorded for it understates its own case.

### 2. Does drain-then-decide actually close B1?

**Yes — verified, and it closes it more cleanly than my remedy would have.** Re-ran my cycle-0
reproduction (600 ms handler; newcomer subscribing 150 ms into the drain) unchanged:

| build | result |
|---|---|
| base `963bde5` | `subs=1 unsubs=0  live cbs=1  late_calls=1` — newcomer receives |
| `aa72813` (cycle 0) | `subs=2 unsubs=1  live cbs=0  late_calls=0` — **newcomer silently dead** |
| `6d74d53` (now) | `subs=1 unsubs=0  live cbs=1  late_calls=1` — newcomer receives |

`subs=1` is the point: because `provider_subscribed` is never published false across the drain, the
newcomer takes the `if (ts.provider_subscribed) return ts.schema_arrival;` fast path and no second
provider subscription is created at all. My transition mutex would have serialised a redundant
subscribe/unsubscribe pair; this makes the pair not happen.

### 3. Is the residual window accurately described as README limit 7?

**Half of it is accurate. The other half is wrong by orders of magnitude — see the blocking finding
below.**

- *"Between that decision and `provider->Unsubscribe`"* — accurate. That is a `lock_guard`
  destructor, a `vector::empty()` and a virtual call, bounded only by preemption. Measured:
  **876,000 deliveries to a live subscription under an adversarial churn thread doing
  Subscribe/Unsubscribe in a tight loop on the same topic, 0 silent losses.**
- *"and between two concurrent first-`Subscribe`s"* — **not a few-instruction window.**
  `EnsureProviderSubscription` releases `mu` across `provider->Subscribe`, so this window is the
  entire duration of the provider call. Measured over 400 trials per setting: **1/400 duplicate
  provider subscriptions when the provider does no work, 400/400 at 50 microseconds, 400/400 at
  500 microseconds.** Fast DDS `create_datareader` is far above 50 microseconds. Under contention
  this is not a race, it is the outcome.

## BLOCKING (re-check)

### RB1 — the second half of README limit 7 is deterministic, not a few-instruction race, and each provider turns it into a different visible failure (confidence: high, measured; code is pre-existing, the characterisation is new)

Two threads whose `Subscribe` calls are the *first* for a topic both observe
`provider_subscribed == false`, both release `mu`, and both call `provider->Subscribe`. Measured
above: 400/400 at 50 microseconds of provider work. Consequences by provider:

- **Fast DDS** — `FastDDSPubSubProvider::Subscribe` guards with
  `if (ts.reader) throw kInvalidArgument "already subscribed to: X"`. The loser's
  `Subscriber::Subscribe` catches, rolls back, retires and **rethrows**, so a caller that did
  nothing wrong gets a spurious `kInvalidArgument` from a valid fan-out subscribe. Loud, but wrong,
  and deterministic under contention. Reachable in the gateway: one `Subscriber` shared by every WS
  session, two sessions subscribing to the same new topic.
- **In-process provider** — `slot.callback = std::move(callback); slot.resolver.reset();` replaces
  silently. The two multiplex lambdas are equivalent (same `Fanout`, same token), so delivery
  survives — but the `resolver.reset()` reports **`kSubscriptionEnded` to the first caller's live
  `SchemaArrival`**. A wrong answer on a subscription that is not ended, with nothing logged.

This is not a request to serialise the teardown side — the adjudication above agrees that must not
be locked. **The subscribe side is different and is safe to serialise:** `provider->Subscribe` does
not wait for an in-flight delivery, so a "first-subscribe in progress" flag plus a condition
variable on `mu` (released while waiting) closes it without touching the gate->provider cycle. The
one shape it could deadlock on — a provider that delivers synchronously from inside `Subscribe` to a
handler that subscribes to the same topic — is exactly the caveat limit 7 already records, and it
would be a loud hang under the suite's `TIMEOUT`.

**Fix (one line of intent):** guard the first-`Subscribe` transition with a per-topic in-progress
flag and a condvar on `mu` so the second caller waits and then takes the `provider_subscribed` fast
path; and split limit 7 so the two windows are described separately, since only one of them is a
few-instruction race.

## SHOULD-FIX (re-check)

### RS1 — the recorded reason for declining the transition mutex names the wrong mechanism (confidence: high)

README limit 7 says the cycle becomes reachable only with "a driver that delivers synchronously from
`Subscribe`". For the teardown side the cycle needs only `provider->Unsubscribe` waiting for its
in-flight delivery, which `provider.hpp` mandates and Fast DDS implements — so it is reachable now.
A future maintainer reading the current text could conclude the mutex is safe to add against today's
providers. State the actual edge.

## Cycle-0 findings: verification

- **S1 — fixed, and the new text is true of every path.** I re-walked all four: `Unsubscribe` copies
  the gate `shared_ptr` under `mu` and calls `RetireAndDrain` after the `lock_guard` scope ends;
  `~Subscriber` gathers under `mu` and drains outside, with the transition in a separate later
  critical section; `Subscribe`'s rollback does `lock.unlock()` first; the delivery path takes the
  gate then `mu`. No path acquires a gate while holding `mu`, and no lock of this file's is held
  across a provider call. I **agree** with skipping the debug assertion — `std::mutex` cannot be
  queried, and a thread-local ownership flag would be new machinery for an invariant with four call
  sites. One gap in the published text: it says nothing about **gate -> gate**, which is a real edge
  across two `Subscriber` objects (a handler holding X's gate blocks on Y's) and is the mechanism of
  the owner-sanctioned cross-`Subscriber` hang; adding "gates of different `Subscriber`s are
  unordered — that is the published residue" would make the note complete.
- **S3 — fixed and correct.** The `catch (...)` is inside the `lock_guard` scope, so the gate is
  released normally on the throw path, the `DeliveryScope` still unwinds, the fan-out continues to
  later subscribers, and nothing reaches the provider thread. Nothing escapes untranslated.
- **S2 — declined; I accept the decline.** The owner ruling ("a handler that cancels any
  subscription on its own subscriber gets an immediate return") is a direct answer to the question I
  raised, and my narrowing contradicts it by construction; it would also re-open
  `CrossCancellingDeliveriesDoNotDeadlock`. The consequence I objected to remains published in
  `subscriber.hpp`, which is what I asked for in substance. No push-back.

## Did I break the new `retiring` mechanism? No — and I tried

Extended the cycle-0 harness with two **duplicate-canceller** threads that race the owning thread on
the very same id, and made each canceller — winner or loser — assert the promise at its own return
point (`running == 0` for that subscription).

| build | callback invocations | duplicate cancels | owner returned while running | duplicate returned while running | post-return invocations |
|---|---|---|---|---|---|
| `6d74d53` | 178,832,253 | 15,016 | **0** | **0** | **0** |
| `aa72813` (control) | 179,241,174 | 15,045 | **413** | **445** | 0 |

The control is the point: before `retiring`, *whichever* thread lost the race took the no-op branch
and returned while the handler was still running — 858 observed violations. The `retiring` map
removes them. The defect it fixes is real, and my cycle-0 385M run could not see it because that run
had no duplicate cancels.

Analytically I could not break it either. The two states are genuinely disjoint, not racing: the
erase from `subscription_topic` and the insert into `retiring` happen in **one** `mu` critical
section, so exactly one thread is the winner and every other observer sees `retiring`; the erase
from `retiring` happens only after the drain has returned, so an id absent from both maps is one
whose drain is complete, for which returning at once is correct. The gate is held in the map by
`shared_ptr`, so it outlives the entry's removal from the fan-out and a waiter that copied it under
`mu` cannot be left holding a dangling gate. A waiter blocks only for as long as the winner's
handler runs, which is the published semantics; it holds no lock while waiting, so it adds no cycle
(and a waiter that is itself inside a delivery on this `Subscriber` skips, per the carve-out). The
core guarantee is unaffected by this cycle — the delivery path changed only by S3's `catch` — and
the numbers above re-confirm it.

## Also checked

- **Both deadlock controls are still capped.** `enable_testing()` instead of `include(CTest)` does
  not affect an explicit per-test property; `gtest_discover_tests(conformance_caller_tier ...
  PROPERTIES TIMEOUT 60)` is unchanged, and all three hang-controls live in that target.

## Nits (re-check, one line each)

- `ADuplicateCancelWaitsForTheDrainInProgress` and `ASubscribeDuringADrainKeepsItsProviderSubscription`
  sequence the critical moment with a bare `sleep_for(100ms)`; if it is short the roles simply swap
  or the newcomer arrives early and the case passes **without exercising the branch it names** —
  false green only, but the file's own rule is "latches, never sleeps".
- `ACancelOfAFullyRetiredIdReturnsWithoutWaiting` cannot redden under any plausible mutation: an
  unknown id has no gate to wait on, so "the branches were collapsed" is not a state the code can
  reach; it documents rather than controls.
- The swallowed callback exception is now completely invisible — no counter, no log, no hook; §5.3
  makes it the subscriber's fault, but a debug-build counter would cost nothing.
- `Subscribe`'s rollback path retires its gate without publishing it in `retiring`, so a concurrent
  cancel of that id during the rollback drain takes the silent no-op branch; only reachable from
  inside the callback itself, which is the published carve-out anyway.
- `~Subscriber` neither consults nor clears `retiring`, so a subscription mid-retirement on another
  thread is not in the destructor's drain list — already UB by object lifetime, but the asymmetry is
  worth a comment.
- If `RetireAndDrain` ever threw (only `std::mutex::lock` failing), the winner's `retiring` entry
  would leak; benign — later cancels of that id lock a free gate and return — but it is an
  unreachable-by-construction claim that nothing states.

## RECORD (re-check, paperwork, non-blocking)

- README limit 7 attaches the "synchronous delivery from `Subscribe`" caveat to the teardown cycle;
  it belongs to the subscribe side (see RS1).
- README limit 7 describes the concurrent-first-`Subscribe` window as few-instruction; measured
  400/400 at 50 microseconds of provider work (see RB1).
- The lock-order note in `subscriber.cpp` does not mention gate -> gate across two `Subscriber`
  objects, which is the mechanism of the published cross-`Subscriber` hang.
