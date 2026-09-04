# PDA-DEC-A4 — §7 clause 6 holds at the tier §9 assigns BIND

*Design, revision 2 (2026-09-04). Round PDA-DEC, item A4. Oracle:
`docs/pubsub-interface-spec.md` §7 clause 6, §9, §12.1. Rulings:
`plans/PDA-DEC-rulings.md`, **37 entries** — including the three of 2026-09-04 that
authorise the idempotence amendment and the blocking wait, and publish the
self-unsubscribe carve-out.*

## Summary

`Subscriber` — the tier §9 assigns BIND — invokes a callback **after**
`Subscriber::Unsubscribe` returns, and throws `kInvalidArgument` when asked to
unsubscribe an id that is not live. Frozen §7 clause 6 promises the opposite of the
first; the owner's 2026-09-04 rulings settle both. This item makes the caller tier
honour them, amends §7 clause 6 to say the guarantee binds at every tier the seam
publishes, and gives that tier an oracle so §9's inheritance row stops being blind.

## The class, stated once

Both defects were found by one reader wearing a C# binding author's hat, and they are
one class, not two: **the seam states its delivery contract for `PubSubProvider`, §9
hands BIND `Publisher`/`Subscriber`, and nothing binds the caller tier to that contract
or measures it.** `ProviderConformance` — which encodes §7's clauses — never touches
`Publisher`/`Subscriber` (`integration-tests/pubsub-conformance/src/clauses.cpp:7`);
`CopyAccounting` does, which is why §8's property is the one thing that *is* proven up
there. So the fix is three-part and the third part stops the class recurring: (1) the
caller tier obeys clause 6 and is idempotent; (2) §7 clause 6 says the clauses bind at
both tiers; (3) a caller-tier suite joins §9's oracle row.

Evidence that the class is real and self-propagating: `pubsub/tests/
test_publisher_subscriber.cpp:406` (`UnsubscribeFromInsideCallbackIsSafe`) **asserts the
defect as intended behaviour** — `EXPECT_EQ(second_calls, 1)`, twice. An in-tree test
currently defends the bug.

## Design

### 1. A retired subscription cannot be invoked (rung 1)

`Subscriber::Impl::Entry` gains one member, a `std::shared_ptr<Gate>`:

```cpp
struct Gate {
    std::mutex mu;                    // NOT recursive — see §1.2
    std::atomic<bool> retired{false}; // set before the barrier, read under mu
};
```

Three sites, and no fourth:

- **Delivery** (`subscriber.cpp:84-88`) keeps the lock-free COW snapshot load, and per
  entry does `std::lock_guard g(gate->mu); if (gate->retired.load(acquire)) continue;
  entry.callback(...)`.
- **`Unsubscribe`** does its map/COW work under `impl_->mu`, captures the gate
  `shared_ptr`, **releases `impl_->mu`**, then *retires*: `retired.store(true, release)`
  followed by a lock/unlock of `gate->mu` — the **barrier**, whose only job is to wait
  out an invocation already inside it.
- **`~Subscriber`** retires and drains every entry the same way, then calls
  `provider->Unsubscribe` per topic.

The store-then-barrier order is what makes the two interleavings total: a delivery that
read `retired == false` under the gate is waited for; a delivery that had not yet taken
the gate reads `true` and skips. "Retired but being invoked" is not representable, and
that is the memory-safety property — on return the caller may free or unpin its callback
state (owner ruling 2026-09-04, with the §1.2 carve-out).

#### 1.1 The lock order is total: `impl_->mu` < gate < provider

Rev 1 stated this over two resources and silently added two more edges. Both are now
**forbidden**, not handled:

- **No gate is held while another gate is acquired (edge gate→gate).** A
  **per-`Subscriber::Impl`** `thread_local` delivery-depth counter is incremented around the delivery
  loop's invocation. **A thread inside a delivery never blocks on any gate:** its
  `Unsubscribe` performs the `retired` store and **skips the barrier**. The
  cross-cancelling ABBA — two concurrent deliveries on two topics of the same
  `Subscriber`, ordinary on Fast DDS's listener-per-reader — therefore has no cycle to
  form. The counter is per-thread scratch, not shared mutable state, and does not touch the
  2026-09-03 isolation claim. *(PM correction at `aa72813`, A4-DEBT-9 — see progress log.)*
- **No gate is held while the provider is entered (edge gate→provider).** The barrier is
  a scoped block that **ends before** `provider->Unsubscribe(segments_to_unsub)`
  (`subscriber.cpp:205-206`) and before the destructor's per-topic call. Never a
  function-scoped `lock_guard`: Fast DDS's `Unsubscribe` waits for its own in-flight
  delivery (`fast_dds_pubsub_provider.cpp:570-574`), which may at that moment be inside
  our gate. This is a scoping mandate on the implementer, not a preference.

The price of the first bullet is stated, published and bounded in §1.3.

#### 1.2 The gate is NOT recursive, deliberately

A `recursive_mutex` would make premise P2 unfalsifiable: a provider that re-entered
delivery for one subscription on one thread would re-acquire and proceed **silently**, so
P2's stop condition could never fire. With a plain `std::mutex` that violation deadlocks
loudly under the suite's ctest `TIMEOUT` — detection rather than masking — and the
*typed* refusal for re-entrancy stays where the owner put it, A3's `kReentrantCall = 10`,
which A4 neither uses nor pre-empts. The self-unsubscribe case that recursion used to
cover is covered by the delivery-depth counter instead, which is why the two changes are
one change.

*Rejected:* an atomic in-flight counter plus condvar. It is cheaper per sample than a
mutex (see DEBT-5 in Risks) but needs its own wait/notify protocol for the same
guarantee, and once the depth counter exists no waiter ever re-enters — so the mutex is
the smaller correct mechanism.

#### 1.3 What the caller may free, and the one shape where it may not

Published, not implied (owner ruling 2026-09-04):

> An `Unsubscribe` **issued from inside a delivery callback on that subscriber** does not
> wait, and in that one shape the caller must not free or unpin callback state on return.

The owner authorised this carve-out for the self shape — *"cancel cannot wait for the
frame it is already in"*. §1.1's forbid widens it by one step, from *this subscription's
own frame* to *any delivery frame on this thread*, because the wait that would be
required is exactly the wait that closes the ABBA cycle. Same shape ("issued from inside
a handler"), same reason, one published sentence. **STOP-AND-ASK** if review or the owner
holds the widening needs its own authorisation — do not narrow it back to the self shape
locally, because that reinstates the deadlock.

### 2. Unsubscribing something that is not live is a no-op (rung 1, at the other end)

`Subscriber::Unsubscribe` with an unknown id returns silently; the throw at
`subscriber.cpp:177-181` is deleted. Authorised by the owner's 2026-09-04 ruling as the
ninth amendment, landing inside A4: a foreign-runtime finaliser calls it unconditionally
and cannot let an exception escape. The bad state removed is not "unknown id" but
**"teardown throws"**, at both tiers. Rev 1's premise P3 is discharged.

### 3. The contract text (authorised amendments only)

- **§7 clause 6** — the guarantee binds at **every tier this seam publishes**; "no further
  callback" means **no invocation of that subscription's callback begins or is in progress
  when `Unsubscribe` returns**; the §1.3 carve-out, published verbatim; and
  **unsubscribing something not live is a no-op, not an error, at every tier**.
  Idempotence is folded into clause 6 rather than appended as a clause 7, so the clause
  count and every existing citation stay stable.
- **Scoped to each tier's own machinery.** The clause is a promise about the tier the call
  was made on. It says nothing about what a provider does with a re-entrant
  `Unsubscribe` — that is A3's (see P5) — and A4 writes no liveness promise covering the
  whole call.
- **§9** — the *Inherits as its oracle* row gains the caller-tier suite for BIND (four
  suites become five). PDA-ABI's cell is unchanged: a driver never sees `Subscriber`.
- Nothing else frozen is touched. §12.3 is **not** amended: review ruled the coverage
  limit belongs in the harness README, where both prior scope rulings put such limits.

### 4. The oracle: a fifth suite in the conformance harness

`integration-tests/pubsub-conformance/src/caller_tier.cpp` → binary
`conformance_caller_tier`, suite **`CallerTier`**, following the precedent the harness set
three times (`CopyAccounting`, `SeamVocabulary`, `Registry`): its own binary, `gtest_main`,
**no transport SDK on the link line** — the narrow link line is itself the guard that this
is a seam property and not a provider's.

Its subject is a probe provider defined in-file that stores the provider callback and
hands the test the trigger, so every case is driven by latches, never sleeps: the timing
window the defect lives in is *made* rather than waited for.

## Corner cases forbidden

| Case | Rung | How |
|---|---|---|
| Callback invoked after its `Unsubscribe` returned | 1 | `retired` store, then barrier; the check is under the same gate |
| Callback invoked after a *concurrent* `Unsubscribe` returned | 1 | the barrier waits out the in-flight invocation |
| Removed-but-still-in-snapshot entry called later in the same delivery | 1 | per-entry check at invocation, not at snapshot time |
| Cross-cancelling deliveries deadlocking (gate→gate) | 1 | a thread inside a delivery never blocks on a gate (§1.1) |
| `Subscriber` deadlocking against a provider's own in-flight wait (gate→provider) | 1 | the barrier's scope ends before any provider call (§1.1) |
| Lock-order inversion | 1 | total order `impl_->mu` < gate < provider, with both new edges removed |
| Teardown throwing on a second/unknown `Unsubscribe` | 1 | no-op at both tiers |
| Callback running after `~Subscriber` returns | 1 | the destructor retires and drains every entry before entering the provider |

Row 4 and the self-unsubscribe case are forbidden **at this tier's gate only**. What a
*provider* does with a re-entrant `Unsubscribe` is P5's, not this table's.

**Handled residue** (each with why it is not forbidden):

- **`Unsubscribe` blocks for as long as a concurrent callback runs.** Not forbiddable:
  frozen clause 6 and the owner's 2026-09-04 ruling *are* that wait.
- **A callback that never returns hangs `Unsubscribe` forever.** Not forbiddable: the seam
  cannot bound foreign callback duration, and a timeout would weaken frozen clause 6 into
  "no further callback, probably". The identical exposure exists at the provider tier.
  Disclosed in the harness README.
- **An `Unsubscribe` issued from inside a delivery does not wait** (§1.3). Not
  forbiddable: the wait is the deadlock. Published, not implied.
- **A subscription id is meaningful only to the `Subscriber` that issued it**; ids are
  per-instance counters from 1, so passing one to the wrong instance silently addresses a
  stranger's subscription. Not forbiddable here: globally unique ids need process-global
  state the 2026-09-03 isolation ruling keeps out of the seam; a per-instance random tag
  is probabilistic, not structural; a typed handle is new public surface and still
  forgeable across an ABI. Predates the item, unchanged by it, disclosed in header + README.

## Premises and stop conditions

- **P1 — the provider tier already honours clause 6.**
  `ProviderConformance.NoDeliveryAfterUnsubscribeReturns` is green on every subject.
  **STOP-AND-ASK if false:** a provider delivering after its own `Unsubscribe` returns is a
  divergence owned by the 2026-08-31 ruling. Do not let the caller-tier gate mask it.
- **P2 — "one callback at a time per subscription" (`provider.hpp:130-132`) holds.**
  **STOP-AND-ASK if false**, and §1.2 is what makes the stop condition able to fire: a
  provider re-entering delivery for one subscription on one thread now **deadlocks under
  the suite's `TIMEOUT`** instead of proceeding silently. Report it; do not make the gate
  recursive to get past it.
- **P5 — the provider tier's answer to a re-entrant `Unsubscribe` is unresolved, and is
  A3's.** `Subscriber::Unsubscribe` calls `provider->Unsubscribe` when a topic's last entry
  goes; issued from inside that subscription's own callback that is a re-entrant provider
  `Unsubscribe` from inside a provider delivery, which today **deadlocks deliberately** on
  the loopback (`in_process_provider.cpp:248,270-275`), self-waits undocumented on Fast DDS
  (`fast_dds_pubsub_provider.cpp:570-574`) and is safe on XRCE. **This item changes none of
  it and claims nothing about it.** `kReentrantCall = 10` is the owner's allocation for
  A3's answer. **STOP-AND-ASK** if the implementer is tempted to fix, refuse or paper over
  it here — including by using `kReentrantCall`. Disclosed in the harness README beside the
  other two limits.
- **P4 — subscription ids are never reused within one `Subscriber`** (`next_id` is a
  monotonic 64-bit `fetch_add`; erasure returns nothing to a pool). If they were, "unknown
  id is a no-op" would silently become "unsubscribes a stranger". Pinned by
  `CallerTier.AReleasedIdIsNeverReused`.
- **P6 — the §1.3 widening stands unless contested.** See §1.3's stop-and-ask.

## Forcing-test mapping

Suite `CallerTier`, binary `conformance_caller_tier`. **Inner loop: `ctest -R
'CallerTier\.'`** — the whole suite, never a single case, because the controls below are
what stop a broken instrument from greening the guard.

| Case | Turns green via | Red today, for the right reason |
|---|---|---|
| **`CallerTier.NoCallbackAfterUnsubscribeReturns`** *(primary)* | §1 per-entry check at invocation | S1's callback is held mid-delivery; main unsubscribes S2 and returns; today S2 is in the loaded snapshot and is invoked afterwards → `s2_called` is true |
| **`CallerTier.UnsubscribeWaitsForAnInFlightDelivery`** *(primary)* | §1 barrier | S1's callback is running on another thread; today `Unsubscribe` returns while it is still inside → `callback_exited == false` at return |
| **`CallerTier.UnsubscribeOfAnUnknownIdIsANoOp`** *(primary)* | §2 | today throws `PubSubError(kInvalidArgument)` |
| `CallerTier.SelfUnsubscribeInsideItsOwnCallbackReturns` | depth counter (§1.1) | control: without it, the non-recursive gate self-deadlocks and dies on the ctest `TIMEOUT` |
| `CallerTier.CrossCancellingDeliveriesDoNotDeadlock` | depth counter (§1.1) | control on **edge A** — two threads delivering two topics, each callback cancelling the other's subscription. Reddens (hangs) on any build that lets a thread inside a delivery block on a gate |
| `CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` | order rule | control on the `mu`↔gate edge |
| `CallerTier.ALiveSubscriptionStillReceives` | — | control that the gate did not simply silence delivery |
| `CallerTier.AReleasedIdIsNeverReused` | — | pins P4 |

*(PM correction at `aa72813`: **10 cases shipped, not the 8 tabled** — see progress log.)*

**Edge B has no unit control** — it needs a provider that waits on its own in-flight
delivery, which the in-file probe is not and `conformance_caller_tier` deliberately cannot
link. It is forbidden by scope, and its live check is `ctest -R 'ProviderConformance\.'`
against the Fast DDS subject, where a gate held across `provider->Unsubscribe` hangs. Said
plainly rather than covered by a probe that could only fail an assertion written to fail
on it (DEBT-1): **falsification of this design rests on the three primaries being red
today and on the two deadlock controls**, and that sentence goes in the README.

**Regression check, free and mechanical:** `ctest -R 'CopyAccounting\.'` (no copy added),
`ctest -R 'ProviderConformance\.'` (provider tier unchanged, and edge B's live check), and
`ctest -R 'SubscriberTest\.'`, whose two defect-pinning cases are rewritten below.

## Risks / Unknowns

- **A deliberately lock-free hot path is reversed, and the reversal is accepted
  unmeasured.** `subscriber.cpp:31-34` records the fan-out being made lock-free (*"an
  atomic load replaces a mutex round trip on every sample"*); the delivery-cost budget this
  seam works to is the measured **1.4 ns per call** at `provider.hpp:109-113`. This design
  adds one uncontended `std::mutex` acquire/release per entry per sample — on MSVC a
  meaningful multiple of 1.4 ns, though far below the ~110 ns regression that budget was
  written about. Taken anyway: frozen clause 6 requires a wait, and no mechanism that waits
  is free. Not measured, because `CopyAccounting` counts copies rather than locks and a
  throwaway probe would be measuring the standard library. **Stated plainly rather than
  hidden**; the non-recursive gate (§1.2) is the cheaper of the two mutexes, and the
  in-flight-counter alternative is named in §1.2 if the number ever matters.
- **Caller-tier coverage is clause 6 and idempotence only.** §7's other clauses stay
  measured at the provider tier alone; the limit is published in the harness README, per
  the narrow-claim-stated-honestly preference the 2026-09-03 ruling licenses a design to
  infer. Not an owner question.
- **The §1.3 widening** is the one thing in this revision the owner has not literally
  ruled on; see P6. It is disclosed in the brief's Risks, not asked as a decision.
- **No coexistence window.** No shim, no dual path, no deprecation; the old behaviour and
  the header paragraph asserting it are deleted in this change. Nothing here is scheduled
  for deletion by a later stage.
- **New public surface: 0 of 3.** Behaviour, contract text and tests only.
- **Expected net lines: +≈360 / −≈45** — down from rev 1's +≈410 because DEBT-1's probe
  case is dropped rather than given a shared instrument.

## Files-to-touch

| Path | Change |
|---|---|
| `docs/pubsub-interface-spec.md` | §7 clause 6 amended (both tiers, quiescence, the §1.3 carve-out, idempotence, tier-scoping); §9 oracle row gains `CallerTier` |
| `pubsub/include/fletcher/pubsub/subscriber.hpp` | delete the "does NOT guarantee … intentional and by design" paragraph; state clause 6, idempotence, the blocking wait, the §1.3 carve-out, and the id-scoping hazard |
| `pubsub/src/subscriber.cpp` | `Gate` on `Entry`; gate check in the fan-out loop; per-`Impl` `thread_local` delivery depth; `Unsubscribe` retires after releasing `mu`, in a scope that **ends before** `provider->Unsubscribe`; unknown id → no-op; destructor retires and drains before entering the provider |
| `pubsub/tests/test_publisher_subscriber.cpp` | two cases rewritten (below) |
| `integration-tests/pubsub-conformance/src/caller_tier.cpp` | **new** — the `CallerTier` suite and its probe provider |
| `integration-tests/pubsub-conformance/CMakeLists.txt` | `conformance_caller_tier` target + `gtest_discover_tests` with `TIMEOUT 60` (a hang here is a deadlock control firing) |
| `integration-tests/pubsub-conformance/README.md` | the fifth suite and what it claims; the falsification sentence above; four disclosed limits — unbounded callback, the §1.3 carve-out, P5's provider-tier re-entrancy residue, id scoping; the caller-tier coverage limit |

## Files-to-delete

- `SubscriberTest.UnsubscribeUnknownIdThrows`
  (`pubsub/tests/test_publisher_subscriber.cpp:339`) — **replaced** by
  `SubscriberTest.UnsubscribeUnknownIdIsANoOp` and `CallerTier.UnsubscribeOfAnUnknownIdIsANoOp`.
- `SubscriberTest.UnsubscribeFromInsideCallbackIsSafe` — rewritten wholesale as
  `…TakesEffectImmediately`. Both `EXPECT_EQ(second_calls, 1)` assertions go
  (`:434-436` **and** `:438-440`, where the second publish's expectations also change to
  `first_calls == 2`, `second_calls == 0`), together with the explanatory comment at
  `:402-405`. No replacement for the old guarantee: **behaviour gone, disclosed** — it was
  the defect.
- `pubsub/include/fletcher/pubsub/subscriber.hpp:64-72`, the paragraph declaring the late
  callback *"intentional and by design … not a bug"* — **no replacement; the statement was
  false against frozen §7 clause 6.**
- Rev 1's `CallerTier.StaleSnapshotProbeIsDetected` — **never written**; replaced by the
  README's plain statement of what falsifies this design (DEBT-1).
