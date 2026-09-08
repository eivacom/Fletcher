# PDA-DEC-AG1 — code review, cycle 3 (scoped re-review)

Diff base `ff714dc`, working tree (uncommitted). Scope: the increment that answers
cycle-2's should-fix **A** (door before the wait), **B** (per-`Subscriber` absorbed
counter) and **C** (typed `DeliveryChannel` constructor), plus the new caller-tier
case.

Out of scope by instruction, and unchanged by this increment: the twelve doors
themselves, the dispatch-under-mutex argument, the delivery-frame stack, and
`DeliveryChannel`'s noexcept/copyable properties. Re-read only far enough to confirm
the increment did not disturb them; it did not.

**Verdict: blocking 0.**

---

## Verification of the three claims

**A — the reorder is sound.** `RefuseIfInsideDeliveryOn(provider.get(), …)` now sits
between the already-subscribed fast path and `provider_cv.wait`, at
`pubsub/src/subscriber.cpp:316`.

- *Lock state on the throw path.* The door throws with `mu` held via the caller's
  `unique_lock`. `Subscriber::Subscribe`'s `catch (…)` opens with
  `if (!lock.owns_lock()) lock.lock();`, so the rollback runs correctly whether the
  throw came from the door (lock held) or from `provider->Subscribe` (lock dropped).
  The lock-order rule ("never acquire a gate while holding `mu`") is respected: the
  rollback unlocks before `RetireAndDrain`.
- *Lost wakeup.* None. The door precedes `ts.provider_subscribe_in_progress = true`
  and the `InProgressGuard`, so a refusal sets no flag and owes no `notify_all`.
- *`ts` read after a state change the wait protected.* No. `ts` is a reference into
  `topics`, which is never erased; the door reads no `ts` field at all.
- *Is the post-wait re-check still sufficient?* Yes, and the door needs no repeat.
  The answer the door computes is a property of *this thread's* thread-local delivery
  stack, and a thread blocked in `provider_cv.wait` cannot enter or leave a delivery
  frame. The door's answer therefore cannot change across the wait in either
  direction.
- *A caller that now sees a refusal where it was previously served.* One shape
  exists: a handler subscribing to topic T while another thread is mid
  `provider->Subscribe(T)`. Under the old order it waited and was served from the
  cached arrival; now it is refused. That is the shape the reorder exists to fix — on
  a provider that dispatches under its instance mutex the wait is a self-wait — and
  §6 clause 6 makes refusal the seam-wide answer. It is loud and typed. Filed as a
  nit below only for the timing-dependence it leaves.

**B — the counter narrowing is lifetime-correct.** `Impl::absorbed` is a
`shared_ptr<atomic<uint64_t>>`; `EnsureProviderSubscription` copies it into
`absorbed_here` under `mu` and the fan-out lambda captures it **by value**
(`[fanout, token, absorbed_here]`, `subscriber.cpp:351`), exactly as it does
`identity`. A delivery in flight when the `Subscriber` is destroyed therefore has a
live counter to write to. Memory ordering is adequate for every reader in the tree:
`caller_tier.cpp:1068-1070` brackets on the same thread the delivery runs on
(program order), and the process-wide `AbsorbedTotal()` reader in `clauses.cpp:582`
polls in a bounded loop, where relaxed coherence suffices. The file-local
`AbsorbedFanoutCounter()` is gone (grep: no hits anywhere in the tree).

**C — the tag does the work claimed.** The two constructors differ in arity, not just
in first-parameter type, so there is no overload-resolution hazard; `RawToken`'s
`explicit` default constructor blocks the accidental `DeliveryChannel({}, p, cb)`.
All four real push sites now pass a provider `this` and get the implicit
derived→base adjustment. Both ends of the identity pair line up today: every door
writes `static_cast<const PubSubProvider*>(this)` and `subscriber.cpp:316` /
`subscriber.cpp:664` ask with `impl_->provider.get()`, all base pointers. The three
raw-tagged sites are genuine — none has a `PubSubProvider` object to name (XRCE test
hook over a bare `Impl`; Fast DDS unit-test and benchmark helpers over a file-local
static).

**The new test is a real control, and the instrument carries a cap.**
`conformance_caller_tier` has `PROPERTIES TIMEOUT 60`
(`integration-tests/pubsub-conformance/CMakeLists.txt:328-329`), so red-by-hang is
reported rather than hung. The interleaving is forced, not lucky: `ProbeProvider::
Subscribe` sets `subscribe_parked` *after* `Subscriber` has set
`provider_subscribe_in_progress = true` and *before* it parks, and the handler waits
on that latch — so with the door after the wait, the handler blocks on a flag only B
can clear while B blocks on `in_flight_ != 0`, which is the claimed deadlock. On a
correct tree every wait in the test is bounded. Ran the target 5× shuffled: 21/21
each time, `TheCallerTierDoorIsReachedBeforeItCanBlock` at 0 ms.
`conformance_inprocess` 16/16.

---

## Findings

### should-fix S1 — the identity pair is typed at one end only (confidence: medium; forbidding direction)

Cycle-2 finding C made the **push** side typed. The **ask** side did not move:
`internal::RefuseIfInsideDeliveryOn` / `InsideDeliveryOn` still take `const void*`,
so each of the thirteen ask sites carries a hand-written
`static_cast<const PubSubProvider*>(this)` that a new door can simply omit —
`RefuseIfInsideDeliveryOn(this, "…")` compiles and converts a derived `this` straight
to `void*`. Today every provider has `PubSubProvider` as its sole non-virtual base, so
the addresses coincide and nothing is wrong; the moment one does not, the door's
answer is silently *wrong in the serving direction*: a re-entrant call is served where
it should be refused, which on the loopback is a deadlock under `mu` and on Fast DDS a
hang on the reader mutex. That is the same "only *incidentally* the same address"
argument the new constructor's own doc comment makes — it just was not applied to the
other half of the pair.

The `core`→`pubsub` layering rule does not block the fix; only the *core* header must
stay `void*`. Add one inline wrapper in a pubsub-tier internal header and use it at
the thirteen sites:

```cpp
// pubsub/include/fletcher/pubsub/internal/provider_delivery_frame.hpp
namespace fletcher::internal {
inline void RefuseIfInsideDeliveryOn(const PubSubProvider* p, const char* method) {
    RefuseIfInsideDeliveryOn(static_cast<const void*>(p), method);
}
[[nodiscard]] inline bool InsideDeliveryOn(const PubSubProvider* p) noexcept {
    return InsideDeliveryOn(static_cast<const void*>(p));
}
}
```

The XRCE test hook (`xrce_dds_pubsub_provider.cpp:1147`) keeps the `const void*` form,
matching its `RawToken` channel — self-consistent, as the header already argues.

### should-fix S2 — the refused `Subscribe` still builds a subscription and then rolls it back (confidence: high; design economy)

The door fires inside `EnsureProviderSubscription`, i.e. *after*
`Subscriber::Subscribe` has already: burned an id from `next_id`, inserted
`subscription_topic[id]`, COW-rewritten the topic's `EntryList` with a fresh `Gate`,
and — via `try_emplace` — permanently created a `topics[key]` node that nothing ever
erases. The refusal then unwinds into the `catch (…)` rollback, which publishes a
retirement, unlocks, and retires-and-drains a gate no one ever held; when the refusal
came from a handler on *this same* `Subscriber`, it additionally defers a sweep to
delivery depth 0 for an entry that was never visible to anybody. Every step is
machinery to undo a state the code could have refused at the door.

No wrong answer today — I traced the rollback for both the same-`Subscriber` and
cross-`Subscriber` shapes and both are benign — but this is the rollback path that
took four fix cycles to get right being exercised, on every refusal, for nothing.

Acceptable fix: hoist the question to the top of `Subscriber::Subscribe`. Under `mu`,
look `key` up in `topics`; if it is absent or `!provider_subscribed`, call
`internal::RefuseIfInsideDeliveryOn(impl_->provider.get(), "Subscriber::Subscribe (new
topic)")` *before* `try_emplace`/`RewriteEntries`. The refusal then touches nothing,
no id is consumed, no retirement or deferred sweep is exercised on the refusal path —
and `subscriber.hpp`'s "before this Subscriber touches any state" becomes true rather
than contradicted (see RECORD).

---

## Nits

- `caller_tier.cpp:1120` — `(void)probe->subscribe_parked.WaitFor(kBudget)` discards the
  result; if the interleaving never establishes, the case passes vacuously. `ASSERT_TRUE`
  it. (It remains a valid hang-control either way, since an unparked B still deadlocks a
  door-after-wait tree.)
- `DeliveryChannel(nullptr, cb)` compiles and fails only at run time; a
  `DeliveryChannel(std::nullptr_t, PubSubProvider::SubscribeCallback) = delete` overload
  moves it to compile time.
- `DeliveryChannel::RawToken`'s constructor is public in an installed header, so
  "Not for providers" is prose only; its three users are two test helpers and one in-tree
  test hook.
- `AbsorbedCallbackFailures() const` is const only cosmetically — pimpl `operator->`
  yields a non-const `Impl*`. Standard for this pattern; no action expected.
- A re-entrant `Subscribe` to the same topic is refused or served depending on whether
  another thread happens to be mid `provider->Subscribe` for it (fast path before the
  door, door before the wait). Documented, loud and typed — but the answer to one call is
  timing-dependent.

---

## RECORD (paperwork the tree contradicts — non-blocking, PM corrects in place)

- `pubsub/include/fletcher/pubsub/subscriber.hpp`, `Subscribe` doc: "raised at this tier,
  **before this Subscriber touches any state**". The tree consumes `next_id`, inserts into
  `subscription_topic` and `topics`, and rewrites the fan-out before the door, then rolls
  back. Same claim in `docs/pubsub-interface-spec.md` §6 clause 6 ("before any wait and
  before any state is touched"). Either S2 lands or both sentences narrow to "before any
  wait, and before entering the provider".
