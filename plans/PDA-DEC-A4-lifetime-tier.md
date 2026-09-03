# PDA-DEC-A4 — §7 clause 6 holds at the tier §9 assigns BIND

*Design, 2026-09-03. Round PDA-DEC, item A4. Oracle: `docs/pubsub-interface-spec.md`
§7 clause 6, §9, §12.1. Rulings: `plans/PDA-DEC-rulings.md` (34 entries).*

## Summary

`Subscriber` — the tier §9 assigns BIND — invokes a callback **after**
`Subscriber::Unsubscribe` returns, and throws `kInvalidArgument` when asked to
unsubscribe an id that is not live. Frozen §7 clause 6 promises the opposite of the
first; `provider.hpp:156-160` promises the opposite of the second. This item makes the
caller tier honour both, amends §7 clause 6 to say the guarantee binds at *every* tier
the seam publishes, and gives that tier an oracle so §9's inheritance row stops being
blind to it.

## The class, stated once

Both defects were found by one reader wearing a C# binding author's hat, and they are
one class, not two: **the seam states its delivery contract for `PubSubProvider`, §9
hands BIND `Publisher`/`Subscriber`, and nothing binds the caller tier to that contract
or measures it.** `ProviderConformance` — which encodes §7's clauses — never touches
`Publisher`/`Subscriber` (`integration-tests/pubsub-conformance/src/clauses.cpp:7`);
`CopyAccounting` does, which is why §8's property is the one thing that *is* proven up
there. So the fix is three-part and the third part is the one that stops the class
recurring: (1) the caller tier obeys clause 6 and is idempotent; (2) §7 clause 6 says
the clauses bind at both tiers; (3) a caller-tier suite joins §9's oracle row.

Evidence that the class is real and self-propagating: `pubsub/tests/
test_publisher_subscriber.cpp:406` (`UnsubscribeFromInsideCallbackIsSafe`) **asserts the
defect as intended behaviour** — `EXPECT_EQ(second_calls, 1)` after a delivery in which
that subscription was unsubscribed. An in-tree test currently defends the bug.

## Design

### 1. A retired subscription cannot be invoked (rung 1)

`Subscriber::Impl::Entry` gains one member, a `std::shared_ptr<Gate>`:

```cpp
struct Gate {
    std::recursive_mutex mu;   // held for the whole of one invocation
    bool retired = false;      // guarded by mu
};
```

- **Delivery** (`subscriber.cpp:84-88`) keeps the lock-free COW snapshot load, and per
  entry does: `std::lock_guard g(*entry.gate ...); if (retired) continue;
  entry.callback(...)`.
- **`Unsubscribe`** does its map/COW work under `impl_->mu` exactly as today, captures
  the gate `shared_ptr`, **releases `impl_->mu`**, then takes the gate and sets
  `retired = true`.

Two states cease to exist rather than being detected:

- *An invocation of a retired callback.* Retirement and invocation take the **same**
  lock, so "retired but still being called" is not a representable interleaving. A
  delivery holding an older snapshot skips the entry; a delivery that got there first
  makes `Unsubscribe` wait for it. This is the memory-safety property: when
  `Unsubscribe` returns, a binding may free or unpin its callback state.
- *Self-deadlock on re-entrant unsubscribe.* The gate is a **recursive** mutex, so a
  callback unsubscribing **itself** re-enters the lock it already owns and returns; its
  own frame is not a *further* callback. The same delivery loop's *later* entries are
  gate-checked individually, so unsubscribing a **different** subscription from inside a
  callback takes effect within that same delivery — the behaviour
  `UnsubscribeFromInsideCallbackIsSafe` currently forbids.

**Load-bearing ordering rule:** `impl_->mu` is never held while a gate is acquired; a
gate may be held while `impl_->mu` is acquired (a callback calling `Subscribe`). One
direction only, therefore no cycle. There is exactly one gate-acquiring site in
`Unsubscribe` and one in delivery; the rule is a comment plus two live tests
(`SelfUnsubscribe…`, `ReentrantSubscribe…`) under a ctest `TIMEOUT`, which is what turns
a violation into a red rather than a hung job.

*Rejected:* an atomic in-flight counter plus condvar (keeps the hot path lock-free but
needs a `thread_local` marker to get the self case right — more code for a per-entry,
per-sample cost of one uncontended lock). Zero-copy is the seam's published performance
property and is untouched; `CopyAccounting` proves that mechanically and for free.

### 2. Unsubscribing something that is not live is a no-op (rung 1, at the other end)

`Subscriber::Unsubscribe` with an unknown id returns silently. The throw at
`subscriber.cpp:177-181` is deleted. This makes the caller tier agree with the provider
tier's already-published sentence (*"a no-op, not an error, so it is safe to call
unconditionally on teardown"*), which is the only reading a foreign-runtime finalizer
can use: a finalizer cannot let an exception escape, and it cannot know whether
`Dispose` already ran.

The bad state removed is not "unknown id" but **"teardown throws"** — the state a
binding cannot recover from. It disappears at both tiers.

### 3. The contract text (authorised amendments only)

- **§7 clause 6** — amended to: the guarantee binds at **every tier this seam publishes**
  (`PubSubProvider` *and* `Subscriber`); "no further callback" means **no invocation of
  that subscription's callback begins or is in progress when `Unsubscribe` returns**; an
  unsubscribe issued from inside that subscription's own callback does not wait for the
  frame it is in; and **unsubscribing something not live is a no-op, not an error, at
  every tier.** Idempotence is folded into clause 6 rather than appended as clause 7, so
  the clause count and every existing citation stay stable.
- **§9** — the *Inherits as its oracle* row gains the caller-tier suite for BIND (four
  suites become five). PDA-ABI's cell is unchanged: a driver never sees `Subscriber`.
- Nothing else frozen is touched. See Risks for §12.3.

### 4. The oracle: a fifth suite in the conformance harness

`integration-tests/pubsub-conformance/src/caller_tier.cpp` → binary
`conformance_caller_tier`, suite **`CallerTier`**. It follows the precedent the harness
already set three times (`CopyAccounting`, `SeamVocabulary`, `Registry`): its own binary,
`gtest_main`, **no transport SDK on the link line** — the narrow link line is itself the
guard that this is a seam property and not a provider's.

Its subject is a probe provider defined in-file that stores the provider callback and
hands the test the trigger, so every case is driven by latches, never by sleeps: the
timing window the defect lives in is *made* rather than waited for.

## Corner cases forbidden

| Case | Rung | How |
|---|---|---|
| Callback invoked after its `Unsubscribe` returned | 1 | retire flag and invocation share one lock |
| Callback invoked after a *concurrent* `Unsubscribe` returned | 1 | same lock; `Unsubscribe` blocks on the in-flight frame |
| Removed-but-still-in-snapshot entry called later in the same delivery | 1 | per-entry gate check at invocation, not at snapshot time |
| Self-unsubscribe deadlocking on its own frame | 1 | recursive gate — owner re-entry always succeeds |
| Lock-order inversion between `impl_->mu` and a gate | 1 | one-directional ordering rule; `mu` released before any gate |
| Teardown throwing on a second/unknown `Unsubscribe` | 1 | no-op at both tiers |
| Callback running after `~Subscriber` returns | 1 | the destructor retires and drains every entry through the same gate **before** calling `provider->Unsubscribe` |

**Handled residue** (each with why it is not forbidden):

- **`Unsubscribe` blocks for as long as a concurrent callback runs.** Not forbiddable:
  frozen clause 6 *is* that wait. Refusing a concurrent `Unsubscribe` would destroy the
  finalizer path this item exists to fix.
- **A callback that never returns hangs `Unsubscribe` forever.** Not forbiddable: the
  seam cannot bound foreign callback duration, and adding a timeout would weaken frozen
  clause 6 into "no further callback, probably". The identical exposure already exists at
  the provider tier. Disclosed in the harness README.
- **A subscription id is meaningful only to the `Subscriber` that issued it**; ids are
  per-instance counters from 1, so passing one to the wrong instance silently addresses a
  stranger's subscription. Not forbiddable here: globally unique ids need process-global
  state the 2026-09-03 isolation ruling keeps out of the seam; a per-instance random tag
  is probabilistic, not structural, and is not what a memory-safety fix should rest on; a
  typed handle is new public surface and still forgeable across an ABI. This hazard
  predates the item and is unchanged by it — disclosed in the header and the README.

## Premises and stop conditions

- **P1 — the provider tier already honours clause 6.**
  `ProviderConformance.NoDeliveryAfterUnsubscribeReturns` is green on every subject.
  **STOP-AND-ASK if false:** a provider that delivers after its own `Unsubscribe` returns
  is a provider divergence owned by the 2026-08-31 divergence ruling. Do not let the
  caller-tier gate mask it — the gate would partly hide exactly that defect, and the
  seam would ship a guarantee held up by the wrong tier.
- **P2 — "one callback at a time per subscription" (provider.hpp:130-132) holds.** It is
  what makes a per-entry gate at most one-waiter and never a throughput device.
  **STOP-AND-ASK if false:** a provider fanning one subscription out across threads is a
  §5/§7 divergence; the gate stays correct but starts serialising deliveries, which is a
  seam behaviour change nobody asked for.
- **P3 — the authorisation covers exactly §7 clause 6, §9's oracle row, and the
  Unsubscribe idempotence sentence.** The 2026-09-03 ruling authorises *"all eight
  amendments"*; idempotence is the verification's finding #5, *"not among the nine"*, and
  reaches this item by the tracker's scoping. **STOP-AND-ASK before landing** if review
  holds that this needs its own owner authorisation — do not land the code change with
  the spec silent, and do not land the spec change unauthorised.
- **P4 — subscription ids are never reused within one `Subscriber`** (`next_id` is a
  monotonic 64-bit `fetch_add`, and erasure never returns a value to the pool). If they
  were, "unknown id is a no-op" would silently become "unsubscribes a stranger" — worse
  than the defect being fixed. Pinned by `CallerTier.AReleasedIdIsNeverReused`.

## Forcing-test mapping

Suite `CallerTier`, binary `conformance_caller_tier`. **Inner loop: `ctest -R
'CallerTier\.'`** — the whole suite, never the single case, because the four controls
below are what stop a broken instrument from greening the guard.

| Case | Turns green via | Red today, for the right reason |
|---|---|---|
| **`CallerTier.NoCallbackAfterUnsubscribeReturns`** *(primary forcing test)* | §1 per-entry gate check at invocation | S1's callback is held mid-delivery; main unsubscribes S2 and returns; today S2 is in the loaded snapshot and is invoked afterwards → `s2_called` is true |
| **`CallerTier.UnsubscribeWaitsForAnInFlightDelivery`** *(primary)* | §1 gate acquisition in `Unsubscribe` | S1's callback is running on another thread; today `Unsubscribe` returns while it is still inside the callback → observed `callback_exited == false` at return |
| **`CallerTier.UnsubscribeOfAnUnknownIdIsANoOp`** *(primary)* | §2 | today throws `PubSubError(kInvalidArgument)` |
| `CallerTier.StaleSnapshotProbeIsDetected` | *(live negative control)* | a hand-built copy-then-release-then-call fan-out in the test file **must** be flagged by the same detector — a detector that cannot see the old shape is not a detector |
| `CallerTier.SelfUnsubscribeInsideItsOwnCallbackReturns` | recursive gate | control against the naive fix: a plain wait-for-quiescence self-deadlocks and dies on the ctest `TIMEOUT` |
| `CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` | ordering rule | control on the `mu`↔gate order |
| `CallerTier.ALiveSubscriptionStillReceives` | — | control that the gate did not simply silence delivery |
| `CallerTier.AReleasedIdIsNeverReused` | — | pins P4 |

**Suites that must stay green and are the regression check (free, mechanical):**
`ctest -R 'CopyAccounting\.'` (no copy added on the hot path),
`ctest -R 'ProviderConformance\.'` (provider tier unchanged), and the pubsub unit binary
`ctest -R 'SubscriberTest\.'`, whose two defect-pinning cases are rewritten below.

## Risks / Unknowns

- **§12.3's blind-spot list is deliberately not amended.** The caller tier gains an
  oracle for clause 6 and idempotence only; §7's other clauses remain measured at the
  provider tier alone. That limit is published in the harness README, the place the
  2026-09-01 and 2026-09-03 rulings put such limits. **If review holds §12.3 must name it
  too, that is a stop-and-ask to the owner** — §12 is frozen and this item's
  authorisation does not reach it.
- **Scope pressure, named rather than absorbed:** running *all* of §7's clauses at the
  caller tier is the complete cure for the class and is roughly a PDA-DEC-1-sized item;
  it would also likely surface further divergences the 2026-08-31 ruling would then
  require fixing in-round. Brief decision 3 offers it; the recommendation is the narrow
  claim published honestly, per the preference the 2026-09-03 ruling licenses a design to
  infer.
- **No coexistence window.** No shim, no dual path, no deprecation: the old behaviour and
  the header paragraph asserting it are deleted in this change. Nothing here is scheduled
  for deletion by a later stage.
- **Contention behaviour change:** a `Unsubscribe` on thread B now waits for a callback on
  thread A. That is the contract; it is visible to an operator only as a teardown that
  takes as long as the last callback. Brief decision 2.
- **New public surface: 0 of 3.** Behaviour, contract text and tests only. One new test
  binary, which is not public surface.
- **Expected net lines: +≈410 / −≈45.**

## Files-to-touch

| Path | Change |
|---|---|
| `docs/pubsub-interface-spec.md` | §7 clause 6 amended (tier-binding + quiescence + idempotence); §9 oracle row gains `CallerTier` |
| `pubsub/include/fletcher/pubsub/subscriber.hpp` | delete the "does NOT guarantee … intentional and by design" paragraph; state clause 6, idempotence, the blocking wait, and the id-scoping hazard |
| `pubsub/src/subscriber.cpp` | `Gate` on `Entry`; gate check in the fan-out loop; `Unsubscribe` retires under the gate after releasing `mu`; unknown id → no-op; destructor retires and drains before `provider->Unsubscribe` |
| `pubsub/tests/test_publisher_subscriber.cpp` | two cases rewritten (below) |
| `integration-tests/pubsub-conformance/src/caller_tier.cpp` | **new** — the `CallerTier` suite and its probe provider |
| `integration-tests/pubsub-conformance/CMakeLists.txt` | `conformance_caller_tier` target + `gtest_discover_tests` with `TIMEOUT 60` (a hang here is the deadlock control firing) |
| `integration-tests/pubsub-conformance/README.md` | the fifth suite; what it claims; the two disclosed limits (unbounded callback, id scoping); the caller-tier coverage limit |

## Files-to-delete

- `SubscriberTest.UnsubscribeUnknownIdThrows`
  (`pubsub/tests/test_publisher_subscriber.cpp:339`) — **replaced** by
  `SubscriberTest.UnsubscribeUnknownIdIsANoOp` and by
  `CallerTier.UnsubscribeOfAnUnknownIdIsANoOp`.
- The post-unsubscribe assertions in `SubscriberTest.UnsubscribeFromInsideCallbackIsSafe`
  (`:434-436`, `EXPECT_EQ(second_calls, 1)`) and their explanatory comment (`:402-405`) —
  **replaced** by `SubscriberTest.UnsubscribeFromInsideCallbackTakesEffectImmediately`,
  asserting `second_calls == 0` for the delivery in progress. No replacement for the old
  guarantee itself: **behaviour gone, disclosed** — it was the defect.
- `pubsub/include/fletcher/pubsub/subscriber.hpp:64-72`, the paragraph declaring the late
  callback *"intentional and by design … not a bug"* — **no replacement; the statement was
  false against frozen §7 clause 6.**
