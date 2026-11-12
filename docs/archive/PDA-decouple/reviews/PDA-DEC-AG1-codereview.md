# PDA-DEC-AG1 — "the misbehaving callback" — independent code review

**Diff base:** `ff714dc` (working tree, uncommitted; new `core/…/internal/delivery_frame.hpp`,
`pubsub/…/delivery_channel.hpp`, `pubsub/src/delivery_channel.cpp`).
**Reviewer:** independent, fresh context. Nothing committed, stashed or modified.

**Counts:** blocking 0 · should-fix 3 · nit 6 · RECORD 5.

---

## Priority 1 — the deadlock argument

**It holds, and the framing in the item brief is off by one state.** At `ff714dc` the loopback
*already* dispatched under `impl_->mu` (`git show ff714dc:pubsub/src/in_process_provider.cpp`,
`Publish`: `std::lock_guard lock(impl_->mu); … cb(buf.data(), …)`). The "delivery gate" that was
deleted existed only in an intermediate working state, never in the base. Relative to the diff
under review the locking model of the loopback is **unchanged**, and the twelve doors are purely
additive: every re-entrant path that used to reach a non-recursive `mu` and wedge now throws
before the `lock_guard`. This diff strictly *removes* deadlock edges from the loopback.

I attacked the argument along the axes named in the brief:

- **Nested provider / second instance, one thread.** `InsideDeliveryOn` is a linear search of the
  whole frame stack, not a top-of-stack test, so `A → B → A` is refused at A's door with `[A, B]`
  on the stack. Nesting survives.
- **Exception unwinding out of a door.** `RefuseIfInsideDeliveryOn` throws before the `lock_guard`
  is constructed at all twelve sites, and it throws *inside* `TranslateSeamFailure`, whose first
  arm rethrows `PubSubError` untouched (`core/…/status.hpp:142`). Nothing is half-locked and
  nothing is re-wrapped.
- **A callback that spawns a thread which calls in.** The new thread has an empty frame stack, is
  not refused, and blocks on `mu` until the delivery returns — the documented "another thread is
  served, it blocks on the drain". If the handler then joins that thread it hangs; that is the
  unchanged, pre-existing consequence of dispatching under a lock and is loud under the TIMEOUT.
- **Teardown racing delivery.** `Unsubscribe` acquires the same `mu`, so it waits the delivery out;
  unchanged.
- **The depth-0 sweep, which now fires one frame further out.** It can never block on a gate this
  thread holds: the fan-out's per-entry `lock_guard` lives strictly inside the fan-out lambda,
  which is strictly inside `DeliveryChannel::Deliver`'s scope, so "stack empty" implies every gate
  this thread took has been released. Moving the sweep from the fan-out frame to the provider
  frame does not change which locks are held when it runs (both are inside `Publish`'s
  `lock_guard` on the loopback, both are outside `OrderedDelivery::mu_` on Fast DDS).

The one shape that still deadlocks is **cross-instance, two threads** — see should-fix 3. It is
pre-existing, not introduced, but this diff is what newly *promises* it works.

## Priority 2 — the door-before-lock invariant, at all twelve sites

**Audited independently, all twelve pass.** For each I read from the entry point down to the first
lock acquisition:

| Provider | CreateTopic | Publish | Subscribe | Unsubscribe |
|---|---|---|---|---|
| `in_process_provider.cpp` | 187 door → 218 `lock_guard` ✓ | 268 door → 281 `lock_guard` ✓ | 302 door → 308 `lock_guard` ✓ | 343 door → 348 `lock_guard` ✓ |
| `fast_dds_pubsub_provider.cpp` | 300 door → 303 `lock_guard` ✓ | 396 door → 407 `shared_lock` ✓ | 452 door → 455 `lock_guard` ✓ | 564 door → 585 `lock_guard` ✓ |
| `xrce_dds_pubsub_provider.cpp` | 651 door → 660 `lock_guard` ✓ | 799 door → 801 `lock_guard` ✓ | 862 door → 864 `lock_guard` ✓ | 1007 door → 1009 `lock_guard` ✓ |

Nothing between an entry point and its door takes a lock: `TranslateSeamFailure` is a pure
try/catch, `JoinSegments` / `DeclaredSchema::Encode` / `VectorWriteBuffer` are lock-free, and the
Fast DDS `static thread_local std::string name` is per-thread by construction. The demonstrated
error shape (door below the `lock_guard`) does not recur.

`PubSubProvider` has exactly four non-destructor virtuals, so twelve is the complete set.

## Priority 3 — the thread-local delivery frame

Correct on every axis the brief names.

- **Cleared on every exit path.** `DeliveryScope` is RAII with a deleted copy; the pop runs during
  unwinding of a throwing callback. If `push_back` throws in the constructor no object exists, so
  there is no unbalanced pop.
- **Per instance, not global.** The token is the provider's `this`, compared by address, never
  dereferenced. A second instance is not refused — `EveryProviderMethodIsRefusedFromInsideA
  Delivery`'s peer branch asserts exactly that, in the positive direction (`EXPECT_TRUE(ok())`),
  which is the right way round.
- **Nesting.** Whole-stack search; `[A, S, B, S]` behaves.
- **Across threads.** `inline thread_local`, no sharing.
- **Across a DLL boundary.** Would fork; guarded by an explicit STOP-AND-ASK in the header. The
  premise is not *enforced*, though — see nit 2.

## Priority 4 — `DeliveryChannel` `noexcept` and copyable

- **HARD-4 / issue #62 is intact.** The three copy-to-local sites (`in_process_provider.cpp:284`,
  `xrce_dds_pubsub_provider.cpp:305` and `:363`) copy the *channel*, and `DeliveryChannel`'s
  implicit copy constructor copies `callback_` by value, so the executing `std::function` is
  independent of the slot a re-entrant reset would null. `RunReentrantUnsubscribeScenario` still
  drives the real flush path and still asserts `delivery_count == 2`.
- **`noexcept` cannot terminate through the callback.** `Deliver`'s only statements are a null
  test, the scope and the invocation, all inside the `try`. Absorption is unconditional.
- **`noexcept` *can* terminate through the scope's destructor.** See should-fix 1.

---

## Findings

### should-fix 1 — `~DeliveryScope` is implicitly `noexcept`, so the "destroy inside the try" mitigation cannot work (confidence: high)

`internal::DeliveryScope` has no members with throwing destructors, so `~DeliveryScope()` is
`noexcept(true)`. Anything escaping `work()` in the depth-0 sweep therefore calls `std::terminate`
**at the destructor boundary** — it never reaches `DeliveryChannel::Deliver`'s `catch (...)`.

Two comments assert the opposite and will be trusted by the next reader:

- `delivery_frame.hpp`, on `DeliveryScope`: *"A dispatch site that is `noexcept` must destroy its
  scope INSIDE its own `try`: the sweep takes a mutex, and a `std::system_error` from that would
  otherwise terminate instead of being absorbed. `DeliveryChannel::Deliver` does exactly that."*
- `delivery_channel.cpp:34-38`, the block comment justifying the extra `{ }` scope.

The extra brace scope buys nothing. Today the only deferred work is
`{ lock_guard(gate->mu); } owner->Release(id);`, whose sole throw is resource exhaustion, so the
practical exposure is small — but the erased `std::function<void()>` queue is explicitly designed
to take arbitrary future work, and the comment invites it.

A second defect is latent behind the same line: if a `work()` ever does throw and the terminate is
suppressed, the remaining entries of `due` are dropped, which un-publishes retirements a
concurrent duplicate cancel is relying on — a silent return while a handler still runs, precisely
the case A4 fix-cycle-3 exists to prevent.

**Acceptable fix:** put the `try`/`catch` where it can actually run — per item, inside the loop in
`~DeliveryScope`:

```cpp
for (const std::function<void()>& work : due) {
    try { work(); } catch (...) { }   // one failed drain must not drop the rest
}
```

and rewrite both comments to say the destructor absorbs, not the dispatch site.

### should-fix 2 — the new refusal is silently lost at the tier every caller actually uses (confidence: high; reachable and silent)

`Subscriber`'s fan-out wraps each handler in a bare `catch (...) { }` (`subscriber.cpp:307-317`),
*inside* the callback that `DeliveryChannel::Deliver` invokes. A `kReentrantCall` raised by one of
the new doors and not caught by the user handler therefore reaches that catch, not the channel: no
status, no log, and **no `AbsorbedTotal` increment**. The observability this item built —
"containing an exception and losing it are indistinguishable from the outside, so the handle
*counts* what it absorbed" — is absent on the path every `Subscriber`-based application takes.

The sharpest reachable instance is not a user mistake at all. `Subscriber::Unsubscribe` was given
its own door (`!internal::InsideDeliveryOn(impl_->provider.get())`, line 587). **`Subscriber::
Subscribe` was not.** A handler that subscribes to a topic it just learned about calls
`EnsureProviderSubscription`, which drops `mu` and enters `provider->Subscribe` from inside the
provider's own delivery frame; the door refuses; the rollback path in `Subscriber::Subscribe`
erases the entry, retires the gate and rethrows; the fan-out `catch (...)` eats the rethrow. The
net observable is: **the subscription silently does not exist, and nothing anywhere records that.**
On XRCE this worked before the diff.

The forbidding-direction reading is the better fix: Fletcher should refuse or divert at *its own*
door rather than enter a provider it knows will refuse, exactly as `Subscriber::Unsubscribe`
already does. Machinery that tolerates a state the caller could be refused at the door is where
this round has been finding its bugs.

**Acceptable fix (either, preferably both):**
1. Give `EnsureProviderSubscription` the same door as `Subscriber::Unsubscribe` — if
   `InsideDeliveryOn(impl_->provider.get())` and the topic is not already provider-subscribed,
   throw `kReentrantCall` from Fletcher's own frame with a message naming the tier, so the
   subscription is never half-created; document it beside the `Unsubscribe` residue in
   `subscriber.hpp`.
2. Bump the shared absorbed counter from the fan-out's `catch (...)` (or let the exception reach
   `DeliveryChannel` and re-enter the loop), so "a handler failed and was contained" has one
   observable at both tiers instead of one.

### should-fix 3 — §6 clause 6 blesses the one shape the loopback cannot serve (confidence: medium-high; pre-existing hazard, newly promised)

Spec §6 clause 6 and `provider.hpp` now say, normatively: *"A callback reaching a **second**
provider instance is not re-entrancy either"* and *"Another **thread** issuing any call during a
delivery is not re-entrancy and is served."* Both are true of the doors. Neither is true of the
loopback's lock:

> Thread 1 is inside A's delivery (holding `A.mu`) and its handler publishes into B.
> Thread 2 is inside B's delivery (holding `B.mu`) and its handler publishes into A.
> Thread 1's stack is `[A]` — B is not on it, so B's door lets it through, and it blocks on `B.mu`.
> Thread 2's stack is `[B]` — same, blocked on `A.mu`. ABBA, no door involved, no timeout.

The same cycle is reachable through the deferred drain: the depth-0 sweep runs *inside*
`DeliveryChannel::Deliver`, i.e. with `A.mu` still held on the loopback, and it can block on a gate
held by a handler running under `B.mu`. The comment in `RetireAndDrain` — *"Blocking in the sweep
is safe precisely because it happens at depth 0: this thread then holds no gate and no `mu`"* — is
true of the `Subscriber`'s `mu` and false of the provider's.

Not introduced by the diff (`ff714dc` dispatched under `mu` too), which is why it is not blocking.
It is a should-fix because the diff is what publishes the guarantee.

**Acceptable fix:** one sentence in §6 clause 6 and in `in_process_provider.hpp` stating that a
handler reaching a second provider instance must not form a cycle back to the first, and that the
loopback holds its instance mutex across dispatch so such a cycle deadlocks rather than being
refused. (Reinstating a dispatch gate on the loopback would remove the hazard outright, but that is
larger than this item's scope and the doors already cover the single-instance case.)

---

## Nits

- Token identity relies on `static_cast<const void*>(Derived*) == static_cast<const void*>(PubSubProvider*)`: the push site passes `this` (derived), `subscriber.cpp:587` asks with `impl_->provider.get()` (base). True for all three providers (single, non-virtual, sole base) but unspecified; `static_cast<const PubSubProvider*>(this)` at the push site would make it exact.
- P1's "every Fletcher component is a STATIC library" is not enforced: `fletcher-xrcedds-pubsub-provider` (`xrcedds-pubsub-provider/CMakeLists.txt:71`) omits the `STATIC` keyword, so `-DBUILD_SHARED_LIBS=ON` forks `g_delivery_stack` silently instead of tripping the STOP-AND-ASK.
- `DeliveryChannel(nullptr, cb)` is constructible, and a null token would match a default-constructed channel's `provider_token_`; no in-tree caller does it, but the constructor could reject it.
- `OrderedDelivery::OfferView` / `DrainLocked` still leave `draining_ = true` forever if the `lk.lock()` *after* a delivery throws; unchanged by this diff (the deleted catches never covered it), but "there is no exception path left" slightly overstates.
- `~Subscriber` does not ask the door question `Subscriber::Unsubscribe` asks, and the skip in `Unsubscribe` is what guarantees `to_unsub` is non-empty, so the tolerated path feeds the terminating one. The ruling wants terminate here, so this is an asymmetry worth a sentence rather than a change.
- Fast DDS's three dispatch sites hold `channel_` as a member rather than copying to a local; safe only because the door now refuses the re-entrant `Unsubscribe`, and worth saying so where the copies are explained.

## RECORD (paperwork the tree contradicts — for the PM, never blocking)

- `pubsub/include/fletcher/pubsub/provider.hpp` — the `SubscribeCallback` bullet ("Re-entrancy: three of the four methods are SERVED … every provider serves them") and the `Unsubscribe` paragraph ("**The one call this seam refuses**") state the superseded ruling; code and spec §6 clause 6 refuse all four.
- `pubsub/include/fletcher/pubsub/in_process_provider.hpp` — "dispatched under a delivery GATE rather than under the instance mutex … `CreateTopic`, `Publish` and `Subscribe` are served from inside a delivery" describes the deleted intermediate; the `.cpp` dispatches under `impl_->mu` and refuses all four.
- `fastdds-pubsub-provider/README.md:514` — the retained bullet "A callback that re-enters the provider (`Publish`, `Subscribe`, `CreateTopic`) from that *first* delivery deadlocks … Re-entering from any later delivery is fine" contradicts the new bullet two lines below it.
- `integration-tests/pubsub-conformance/CMakeLists.txt:296` — names `DeclarePublishAndSubscribeAreServedFromInsideADelivery`, a clause that no longer exists; the shipped one is `EveryProviderMethodIsRefusedFromInsideADelivery`.
- `integration-tests/pubsub-conformance/src/clauses.cpp` — the AG1 header block says "AG1-6 needs neither, and only the loopback's **delivery gate**", and the CMake comment above says the loopback "**used to** dispatch under its instance mutex"; it dispatches under its instance mutex now and has no gate.

---

# RE-REVIEW after fix cycle 2

**Diff base:** `ff714dc`, working tree, uncommitted. Nothing committed, stashed or modified.
**Reviewer:** independent, fresh context, same reviewer as above.

**Counts:** blocking 0 · should-fix 3 · nit 4 · RECORD 1.

## The three should-fixes are closed

**should-fix 1 — the termination hazard: CLOSED.** The `try`/`catch` is now per item *inside*
`~DeliveryScope` (`core/…/internal/delivery_frame.hpp`), which is where it can actually run. I
walked every statement of the destructor for an escape: `pop_back` (noexcept, and balanced —
`DeliveryScope` is the only pusher and is RAII with a deleted copy), `empty()`, default-constructing
`due` (no allocation), `swap` (noexcept), the loop body wrapped per item, and `~due` destroying
`std::function`s whose captures are two `shared_ptr`s and a `uint64_t`. The catch handler is empty,
so the only way it could itself throw is a throwing exception-object destructor. Nothing escapes,
and a failing drain no longer strands the entries behind it. Both comments now say the destructor
absorbs; `delivery_channel.cpp` explicitly states the opposite of what it said before and explains
why. The reasoning is correct as written.

**should-fix 2 — the silent refusal: CLOSED, and the implementer's diagnosis was better than
mine.** The door at `subscriber.cpp:305` sits after the already-subscribed early return (`:282`) and
before `provider_subscribe_in_progress` (`:311`), the unlock and the `provider->Subscribe` call, so
both directions hold: the permitted cached-arrival shape
(`ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock`, same topic, already provider-subscribed) is
served without touching the provider, and no provider *work* precedes the refusal. The refusal is
raised with `mu` held and unwinds into `Subscriber::Subscribe`'s rollback, which re-takes nothing it
does not own, erases the id, removes and retires the entry, and rethrows — I traced the
`owns_retirement` / deferral branch for both "delivery on this Subscriber" (defers; released at
depth 0) and "delivery on another Subscriber over the same provider" (takes a barrier on a fresh
gate, which is free). No half-state survives. The counter is a function-local `std::atomic<uint64_t>`
with relaxed increments — correct for a monotonic counter, no ordering is claimed on anything else,
and single-variable coherence is enough for clause AG1-3's spin-wait. The observation that
`ProbeProvider` has no door is right, and is what makes the caller tier the only place the answer can
be uniform. See should-fix A for the one placement detail I do not think is finished, and should-fix
B for the counter's *scope*.

**should-fix 3 — CLOSED.** Spec §6 clause 6 now states the cross-instance cycle restriction in the
forbidding direction ("A callback must not enter a provider instance that can, directly or
transitively, deliver back into the instance it is running on"), says the doors cannot help, and says
the loopback holds its instance mutex across dispatch. That is exactly the sentence the promise above
it needed.

All five RECORD items from the first review are corrected in the tree (checked individually).

## Findings

### should-fix A — the caller-tier door is one statement too late: a re-entrant Subscribe can still deadlock instead of being refused (confidence: high; reachable, loud rather than silent)

`EnsureProviderSubscription` waits on `provider_cv` at `subscriber.cpp:280` **before** the door at
`:305`. That wait is not provider work, but it is an unbounded wait on a flag another thread clears
only by returning from `provider->Subscribe` — and on the loopback and XRCE the thread inside a
delivery is holding the provider's own mutex while it waits:

> Thread B: `Subscriber::Subscribe(T)`, first on T. Sets `provider_subscribe_in_progress = true`,
> unlocks `mu`, calls `provider->Subscribe(T)`, blocks on the provider's `impl_->mu`.
> Thread A: inside `InProcessPubSubProvider::Publish`, holding that same `impl_->mu` across
> `channel.Deliver`. Its handler calls `Subscriber::Subscribe(T)` on the same Subscriber. A takes
> `mu`, reaches `:280`, sees `in_progress == true`, and waits. B needs the mutex A is holding.
> Neither the caller-tier door nor the provider's door is ever reached.

The same shape on XRCE (the pump thread holds the recursive `impl_->mu` across dispatch). Fast DDS
does not hold `impl_->mu` on the listener thread, so there it merely waits and is then correctly
served or correctly refused.

It is a hang rather than a silent wrong answer, and the wait predates this diff — which is why it is
not blocking. It is a should-fix because this diff is what publishes "this refusal is raised before
any state is touched", and the one shape the door exists to make deterministic is still
nondeterministic: the same call on the same thread is refused or deadlocked depending on another
thread's timing.

**Acceptable fix:** ask the permitted question first, then refuse, then wait —

```cpp
if (ts.provider_subscribed) return ts.schema_arrival;      // permitted, and needs no wait
internal::RefuseIfInsideDeliveryOn(provider.get(), "Subscriber::Subscribe (new topic)");
provider_cv.wait(lock, [&ts] { return !ts.provider_subscribe_in_progress; });
if (ts.provider_subscribed) return ts.schema_arrival;
```

and say in the comment that the door precedes the wait deliberately. The only behaviour this changes
is that a re-entrant new-topic Subscribe which *might* have been served because another thread
happened to win the race is now refused unconditionally — which is the answer the clause promises
anyway.

### should-fix B — `Subscriber::AbsorbedCallbackFailures()` is process-wide where the observer holds the object (confidence: medium; design economy)

The process-wide scope is argued from `DeliveryChannel::AbsorbedTotal()`'s reason: a conformance
clause can only see a `ProviderSubject` and cannot reach the channel. That reason is real one tier
down and does **not** transfer here — every observer of the fan-out counter, in the suite and in an
application, is holding the `Subscriber` whose handler failed (`caller_tier.cpp`'s new clause
brackets `Subscriber::AbsorbedCallbackFailures()` while holding `subscriber`). As shipped, an
application asking "did one of *my* handlers fail?" is also told about every other `Subscriber` in
the process, so the counter cannot answer the question it was added for. It is public surface, so the
scope is expensive to narrow later.

**Acceptable fix:** make it a non-static member reading a per-`Impl` `std::atomic<uint64_t>` (the
fan-out lambda already captures `fanout` and `token`; a `shared_ptr` to the counter is the same
capture cost). Keep a process-wide static only if some caller genuinely cannot reach the instance,
and name that caller. If it stays static, the doc must say the number is not attributable to a
Subscriber and that a bracketed delta is sound only in a single-threaded test.

### should-fix C — the base-cast rule is a convention with no enforcement, and one site already breaks it (confidence: medium)

The header now says, normatively, *"A provider must pass `static_cast<const PubSubProvider*>(this)`,
not a bare `this`"*, and all four real push sites and all twelve doors comply — I checked each
(`in_process_provider.cpp:187/268/302/312/344`,
`fast_dds_pubsub_provider.cpp:300/396/452/493/503/566`,
`xrce_dds_pubsub_provider.cpp:651/799/862/975/1007`). The identity is stable and correct at every one
of them, and the `Subscriber` side asks with `impl_->provider.get()`, a base pointer, at both of its
doors — so the comparison is exact end to end. But `caller_tier.cpp:131` passes a bare derived
`this` while the caller-tier door asks with the base pointer: the exact mismatch the header forbids,
in the very subject that exercises the new door. It works (single non-virtual base at offset 0) and a
mismatch would redden the test rather than hide, so this is not a correctness defect today. It is
evidence that a rule stated in a comment across nineteen sites will not hold.

**Acceptable fix (forbidding direction):** add `DeliveryChannel(const PubSubProvider*,
SubscribeCallback)` as the ordinary constructor and keep the `const void*` one behind a named tag
(`DeliveryChannel::RawToken{}`) for the XRCE test hook that has no provider object. The four real
sites then cannot get the identity wrong, the hook stays possible and is labelled as the exception,
and a paragraph of comment becomes a signature. Fix `caller_tier.cpp:131` either way.

## Nits

- `Subscriber::Subscribe`'s own doc comment never says it can now throw `kReentrantCall`; the new user-visible refusal is documented only at `Unsubscribe` and in the spec.
- `DeliveryScope`'s constructor `push_back` can throw `bad_alloc` inside `DeliveryChannel::Deliver`'s `try`, counting an absorption for a callback that never ran.
- `OrderedDelivery::OfferView` reads `schema_` unlocked when passing it to `channel_.Deliver` (pre-existing, unchanged here).
- Clause AG1-3's `EXPECT_EQ(AbsorbedTotal(), before + 1)` is an exact equality on a process-wide counter; a second absorption anywhere in the process during the bracket reds it. The `entered` guard makes that unlikely, not impossible.

## Verified, no finding

- **Nits 2–6 from the first review are all actioned and correct.** P1 is now enforced rather than asserted: every Fletcher target is explicitly `STATIC` (`arrow-bridge`, `fastdds-pubsub-provider`, `pubsub`, `pubsub-arrow`, `protoc`, `xrcedds-pubsub-provider`) with `core` INTERFACE, so `-DBUILD_SHARED_LIBS=ON` can no longer fork `g_delivery_stack`. The XRCE `STATIC` keyword has no link or ODR consequence — the target was already built static by default in every in-tree configuration, and nothing exports from it. The null-token refusal is correct and reaches no in-tree caller (`DeliveryChannel{}` for the reset paths uses the defaulted constructor, which is untouched); the `draining_` claim is now narrowed to exactly the `lk.lock()` path that is really exposed; the `~Subscriber` asymmetry and the Fast DDS member-channel dependency on the door are both stated where they are needed.
- **Fast DDS `catch` deletions are safe.** `OrderedDelivery`'s three `catch (...) { draining_ = false; throw; }` pairs are removed and every dispatch now goes through `noexcept` `DeliveryChannel::Deliver`, so `draining_` cannot be left set by a callback; the `Take`-wide guard in `data_reader_listener.hpp` is correctly retained and re-described for the non-callback throws it still covers.
- **The depth-0 sweep firing one frame further out is safe on all three providers.** On the loopback the sweep can block on a gate while `impl_->mu` is held, but the loopback serialises deliveries under that same mutex, so no other thread can be inside a delivery to hold the gate; on XRCE there is one pump thread; on Fast DDS the listener threads that can hold a gate cannot block on anything the sweeping thread holds, because every provider call they could make is now refused at a door. The comment's claim is narrower than the truth and does not overstate.

## RECORD (paperwork, for the PM — never blocking)

- `delivery_frame.hpp`'s P1 STOP-AND-ASK still reads as if the "every Fletcher component is STATIC" premise were an assumption; it is now enforced in CMake at every target. Worth one clause saying so, or the next reader re-verifies it as I did.
