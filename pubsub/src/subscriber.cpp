// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/subscriber.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fletcher/core/internal/delivery_frame.hpp>
#include <fletcher/core/status.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {
namespace {

// ── The delivery-depth scope ────────────────────────────────────────
//
// "Is THIS thread currently inside a delivery on THAT Subscriber?" — the one
// question that decides whether Unsubscribe takes the barrier or skips it.
//
// The stack, the scope and the predicate now live in
// `fletcher/core/internal/delivery_frame.hpp`, because the PROVIDER tier asks the
// same question of its own instances (PDA-DEC-AG1) and two thread-locals asking
// it would be two answers. Moved, not duplicated.
//
// The storage is thread-local, but the SCOPE is per Subscriber (owner ruling
// 2026-09-04): the stack holds one identity token per delivery frame, and the
// predicate asks for a specific token. A handler on subscriber X cancelling on
// subscriber Y therefore does NOT skip Y's barrier — Y's caller reads the
// published sentence, believes the wait happened, and is right. A file-local
// depth counter would have made that sentence false in the unsafe direction.
//
// The token is compared by address and NEVER dereferenced: the provider callback
// keeps it alive by shared_ptr, so it outlives the Subscriber it identifies.
using internal::InsideDeliveryOn;

// One per subscription. `retired` is stored BEFORE the barrier is taken and read
// under `mu` at invocation, which is what makes the two interleavings total: a
// delivery that read `retired == false` under the gate is waited for, and one
// that had not yet taken the gate reads `true` and skips. "Retired but being
// invoked" is not a representable state — that is the memory-safety property,
// not a test.
//
// NOT recursive, deliberately: a recursive gate would let a provider that
// re-entered delivery for one subscription on one thread proceed silently, which
// would make provider.hpp's "one callback at a time" unfalsifiable and would
// pre-empt the typed re-entrancy refusal that belongs to a separate item. With a
// plain mutex that violation deadlocks loudly under the conformance suite's
// ctest TIMEOUT. The self-cancellation case a recursive gate used to cover is
// covered by the delivery-depth scope instead.
struct Gate {
    std::mutex mu;
    std::atomic<bool> retired{false};
};

// The third state, between "live" and "gone": ids whose gate has been retired
// but whose drain has not yet been observed to complete. It is what lets a
// duplicate cancel wait for the same drain instead of returning as a no-op
// (owner ruling 2026-09-04).
//
// Held by `shared_ptr` rather than inline in `Impl` for one reason: a delivery
// frame that deferred a release into it must be able to finish that release even
// if the `Subscriber` is destroyed the instant its handler returns.
//
// Its lock is always innermost — taken under the Subscriber's `mu` for publish
// and lookup, alone for the deferred release, and never held across anything
// that can block. Publishing under `mu`, in the same critical section that
// removes the id from the live map, is what keeps the three states disjoint with
// no window between the first two.
class Retirements {
   public:
    void Publish(uint64_t id, std::shared_ptr<Gate> gate) {
        std::lock_guard<std::mutex> lock(mu_);
        map_.emplace(id, std::move(gate));
    }
    [[nodiscard]] std::shared_ptr<Gate> Find(uint64_t id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(id);
        return it == map_.end() ? nullptr : it->second;
    }
    void Release(uint64_t id) {
        std::lock_guard<std::mutex> lock(mu_);
        map_.erase(id);
    }

   private:
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, std::shared_ptr<Gate>> map_;
};

}  // namespace

struct Subscriber::Impl {
    // Identity for the delivery-depth stack. Never dereferenced; held by
    // shared_ptr so the provider callback can keep the token alive past `this`.
    struct Identity {};

    struct Entry {
        uint64_t id;
        SubscribeCallback callback;
        std::shared_ptr<Gate> gate;
    };

    // Immutable snapshot of a topic's subscribers, so delivery copies one shared_ptr instead of
    // scanning every subscription in the process and copying a std::function per match. Rebuilt on
    // Subscribe/Unsubscribe (rare); read on every sample (hot).
    using EntryList = std::shared_ptr<const std::vector<Entry>>;

    // The snapshot lives in its own heap object, owned jointly by the topic state and by the
    // provider callback that delivers to it. That is what keeps delivery off `mu`: the callback
    // holds the fanout directly, so it needs neither the map lookup that used to find it nor the
    // lock that lookup required — an atomic load replaces a mutex round trip on every sample.
    struct Fanout {
        std::atomic<EntryList> entries{std::make_shared<const std::vector<Entry>>()};
    };
    using FanoutPtr = std::shared_ptr<Fanout>;

    struct TopicState {
        std::vector<std::string> segments;
        FanoutPtr fanout = std::make_shared<Fanout>();
        // The provider's schema arrival, cached so fan-out subscribers to the
        // same topic all share it (SchemaArrival is copyable).
        SchemaArrival schema_arrival;
        bool provider_subscribed = false;
        // Set while THIS topic's first Subscribe is inside provider->Subscribe,
        // which runs with `mu` released. See EnsureProviderSubscription.
        bool provider_subscribe_in_progress = false;
        // Outstanding SubscribeSchema watches. The provider's watch is
        // idempotent per topic, so it is the LAST release here that forwards,
        // not the first — the same shape as the fan-out one line up, counted
        // rather than listed because a watch has no callback to address.
        uint32_t schema_watches = 0;
        // The provider's schema-only arrival, cached so a second watch on this
        // topic observes the same one.
        SchemaArrival schema_watch_arrival;
        // Set while THIS topic's first SubscribeSchema is inside
        // provider->SubscribeSchema, with `mu` released. Same flag+cv protocol
        // as provider_subscribe_in_progress, and it is the count alone that
        // cannot replace it: the first caller increments BEFORE it unlocks, so a
        // second caller testing only `schema_watches > 0` would return the
        // arrival that has not been stored yet — a default-constructed one,
        // which reports kSubscriptionEnded for a schema that is on its way.
        bool schema_watch_in_progress = false;
    };

    std::shared_ptr<PubSubProvider> provider;

    std::shared_ptr<Identity> identity = std::make_shared<Identity>();

    // Callback failures THIS Subscriber's fan-out has absorbed. Held by
    // shared_ptr for the same reason `identity` is: the provider-side callback
    // outlives `this` on a provider that has not yet been told to stop, and it
    // must have somewhere to count. Scoped per instance rather than per process
    // because every observer of this number — in the suite and in an
    // application — is holding the Subscriber whose handler failed, and a
    // process-wide total cannot answer "did one of MY handlers fail?".
    std::shared_ptr<std::atomic<uint64_t>> absorbed = std::make_shared<std::atomic<uint64_t>>(0);

    // ── The lock order, as the code actually enforces it ────────────
    //
    //     gate  <  mu  <  (nothing)          and no lock is EVER held across a
    //                                        provider call.
    //
    // The outermost lock is the **gate**, not `mu`: the fan-out holds
    // `entry.gate->mu` across the user callback, and that callback may re-enter
    // `Subscribe`/`Unsubscribe`, which take `mu`. So gate → mu is a real,
    // exercised edge (`CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNot
    // Deadlock` is its control). The rule that keeps the graph acyclic is
    // therefore the reverse of the obvious one:
    //
    //   **Never acquire a gate while holding `mu`.** `RetireAndDrain` must be
    //   called with no lock held at all.
    //
    // The delivery path also holds a gate across whatever the callback does —
    // including `Publisher::Publish`, so a gate can be held across a provider
    // call *by the user*, and nothing here can prevent that. The rule below
    // constrains this file's own calls only: no gate is held when WE enter the
    // provider, because a provider's Unsubscribe may wait for its in-flight
    // delivery, and that delivery may at that moment be about to take the gate
    // we hold (`CallerTier.UnsubscribeDoesNotHoldAGateWhileEnteringTheProvider`).
    //
    // Gates of DIFFERENT `Subscriber` objects are unordered with respect to each
    // other: a handler holding X's gate may block on Y's. That edge is the
    // mechanism of the published cross-`Subscriber` hang (harness README), which
    // is handled residue by owner ruling rather than a defect — a loud hang is
    // preferred over a silent use-after-free.
    //
    // Retire a subscription and wait out any invocation already inside its gate.
    // A thread already inside a delivery on THIS Subscriber skips the barrier —
    // otherwise two cross-cancelling deliveries deadlock on each other's gates.
    // That skip is the one published exception to "you may free on return"
    // (subscriber.hpp; owner ruling 2026-09-04).
    //
    // Returns true if the release of `id` from `retirements` was DEFERRED to the
    // end of this thread's delivery frame — which is how the skip stops
    // un-publishing the drain for every other thread. Only the owner of the
    // retirement may defer or release it; a duplicate cancel that lands in the
    // carve-out simply returns.
    [[nodiscard]] bool RetireAndDrain(uint64_t id, const std::shared_ptr<Gate>& gate,
                                      bool owns_retirement) const {
        gate->retired.store(true, std::memory_order_release);
        if (InsideDeliveryOn(identity.get())) {
            if (!owns_retirement) return false;
            // Owed until this thread leaves its OUTERMOST delivery frame, not
            // until this cancelling frame ends. Ending the cancelling frame is
            // not the event the promise is about: a handler may cancel a SIBLING
            // subscription whose callback is running on another thread (§6 clause
            // 2 permits it, and Fast DDS's listener-per-reader makes it
            // ordinary), and that frame ends first. The GATE is captured, not just
            // the id, because the guarantee is about the gate's lifetime and
            // nothing narrower — the id must stay published until that gate is
            // free, wherever the callback holding it happens to be running.
            //
            // Why the id must stay published at all: the winner would otherwise
            // un-publish the drain the moment it skips, and a cancel of the same
            // id from any OTHER thread would find neither map, take the no-op
            // branch, and return while that callback was still running. That
            // caller is not covered by the published carve-out.
            //
            // Blocking in the sweep is safe precisely because it happens at depth
            // 0: this thread then holds no gate and no `mu`, and a thread inside a
            // delivery never blocks on a gate, so no cycle is representable.
            std::shared_ptr<Retirements> owner = retirements;
            internal::DeferUntilDeliveryDepthZero([owner, gate, id] {
                { std::lock_guard<std::mutex> barrier(gate->mu); }
                owner->Release(id);
            });
            return true;
        }
        // If this ever threw — only std::mutex::lock failing, which the standard
        // reserves for resource exhaustion — the owner's entry would stay in
        // `retirements`; benign, since later cancels then lock a free gate and
        // return at once.
        std::lock_guard<std::mutex> barrier(gate->mu);
        return false;
    }

    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
    // Only maps an id to its topic, so Unsubscribe can find the list to rebuild. The callback
    // itself lives in that topic's EntryList.
    std::unordered_map<uint64_t, std::string> subscription_topic;
    // Ids whose entry has been pulled out of the fan-out but whose drain has NOT
    // finished. It is what tells "this id is being cancelled right now on another
    // thread" apart from "this id is unknown or already fully cancelled": the
    // first waits for the same drain, the second is a silent no-op (owner ruling
    // 2026-09-04). Without it a concurrent duplicate cancel returned while the
    // handler was still running — a second, unpublished exception to the frozen
    // promise, and the one the owner refused to publish.
    std::shared_ptr<Retirements> retirements = std::make_shared<Retirements>();
    // One per Subscriber rather than per topic: a first-Subscribe is rare, so a
    // shared condition variable costs a predicate re-check nobody notices.
    std::condition_variable provider_cv;
    std::atomic<uint64_t> next_id{1};

    // Copy-on-write mutation of one topic's subscriber list. Called with mu held — the lock is what
    // serialises two rewrites, not what publishes the result. Never mutates a published list, so a
    // delivery already iterating an older snapshot stays valid.
    template <typename Mutate>
    static void RewriteEntries(TopicState& ts, Mutate mutate) {
        auto next = std::make_shared<std::vector<Entry>>(*ts.fanout->entries.load());
        mutate(*next);
        ts.fanout->entries.store(std::move(next));
    }

    // Called with mu held. Releases the lock while calling into the
    // provider to avoid deadlock if the provider calls back synchronously.
    SchemaArrival EnsureProviderSubscription(const std::string& key, TopicState& ts,
                                             std::unique_lock<std::mutex>& lock) {
        // Wait out a first-Subscribe already inside provider->Subscribe for this
        // topic. `mu` is released across that call, so without this both callers
        // read provider_subscribed == false and BOTH register: measured 400/400
        // duplicate provider subscriptions at 50 us of provider work — the
        // outcome under contention, not a race. Fast DDS then refuses the loser
        // with kInvalidArgument on a perfectly valid Subscribe; the loopback
        // silently replaces the slot and reports kSubscriptionEnded to a live
        // subscriber's SchemaArrival.
        //
        // Serialising THIS side is safe where serialising the teardown side is
        // not: provider->Subscribe never waits for an in-flight delivery, so it
        // closes no cycle, whereas provider->Unsubscribe is REQUIRED to wait
        // (provider.hpp) and a lock held across it would close
        // lock -> provider -> gate -> mu -> lock. The one shape this can still
        // hang on is a provider that delivers synchronously from inside
        // Subscribe into a handler that subscribes to the same topic — a loud
        // hang under the suite's TIMEOUT, and published in the harness README.
        //
        // ── The already-subscribed fast path, which needs no provider ──
        //
        // Read BEFORE the wait as well as after it. A handler that joins a topic
        // this Subscriber has already subscribed needs no provider call at all,
        // so it must not be made to wait for anything: that is the permitted
        // shape `ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` pins.
        if (ts.provider_subscribed) {
            return ts.schema_arrival;
        }

        // ── The door, BEFORE the wait ──────────────────────────────────
        //
        // **Ordering is the whole of this.** Past this point the call must enter
        // the provider, and from inside that provider's own delivery frame it
        // cannot be served — so it is refused here, by name.
        //
        // It must come before `provider_cv.wait` and not after. The flag that
        // wait blocks on is cleared only by the thread inside `provider->
        // Subscribe`, and that thread returns only once the provider lets it —
        // which, on a provider that dispatches under its instance mutex, means
        // once THIS delivery has returned. A handler that waited there would be
        // waiting for itself: a deadlock on the loopback and on XRCE, in place of
        // a refusal. Found by two reviewers independently; it is the same defect
        // as a door placed after a lock, with a condition variable in the lock's
        // place.
        //
        // Refused at THIS tier rather than left to the provider's own door, for
        // two reasons. It is uniform: `ProbeProvider` and any future provider
        // that forgets its door would otherwise serve a call the seam refuses
        // everywhere else. And it is not silent: the deep refusal used to unwind
        // through the rollback in `Subscriber::Subscribe` into the fan-out's
        // `catch (...)`, which contained it — correctly, per §5.3 — and thereby
        // erased it, leaving the caller believing it held a subscription that
        // does not exist. The containment stays; what changed is that it now
        // counts, and that the refusal is raised before this function touches
        // any state or blocks on anything.

        internal::RefuseIfInsideDeliveryOn(provider.get(), "Subscriber::Subscribe (new topic)");

        // `ts` stays valid across the wait and across the unlock below: nothing
        // ever erases from `topics`, and unordered_map nodes are stable.
        provider_cv.wait(lock, [&ts] { return !ts.provider_subscribe_in_progress; });

        // Re-read after the wait: the thread we waited out may have been the one
        // that established the provider subscription.
        if (ts.provider_subscribed) {
            return ts.schema_arrival;
        }

        std::vector<std::string> segments = ts.segments;
        FanoutPtr fanout = ts.fanout;
        std::shared_ptr<Identity> token = identity;
        std::shared_ptr<std::atomic<uint64_t>> absorbed_here = absorbed;

        ts.provider_subscribe_in_progress = true;
        lock.unlock();

        // Whatever happens, the flag must be cleared and waiters woken, or every
        // later Subscribe on this topic waits forever.
        struct InProgressGuard {
            TopicState& ts;
            std::unique_lock<std::mutex>& lock;
            std::condition_variable& cv;
            ~InProgressGuard() {
                if (!lock.owns_lock()) lock.lock();
                ts.provider_subscribe_in_progress = false;
                cv.notify_all();
            }
        } in_progress{ts, lock, provider_cv};

        SubscriptionResult result = provider->Subscribe(
            segments,
            [fanout, token, absorbed_here](const uint8_t* data, size_t len,
                                           const SharedSchema& schema, const Attachments& att) {
                EntryList entries = fanout->entries.load();
                // One push/pop per sample, not per entry: it marks the whole
                // fan-out frame, so a handler cancelling ANY subscription on this
                // Subscriber skips the barrier rather than blocking on a gate.
                internal::DeliveryScope scope(token.get());
                // Borrowed by every subscriber; a callback that keeps either one copies it.
                for (const Entry& entry : *entries) {
                    // This reverses the deliberately lock-free fan-out recorded
                    // above, and the reversal is MEASURED, not assumed: probed on
                    // MSVC 19.44 x64 /O2 (i7-13850HX, 7 runs, spread <= 2%)
                    // against the 1.4 ns delivery budget at provider.hpp:109-113,
                    // an uncontended std::mutex acquire/release costs
                    // **+11.10 ns per visit** (1.28 -> 12.37), a recursive_mutex
                    // +11.02, and an atomic counter + `retired` +7.42. Taken
                    // anyway: §7 clause 6 requires a wait, and nothing that waits
                    // is free. The cheaper atomic form needs its own wait/notify
                    // protocol on the Unsubscribe side — more machinery, on the
                    // path that is not hot — so the mutex is the smaller correct
                    // mechanism.
                    std::lock_guard<std::mutex> gate(entry.gate->mu);
                    // Checked HERE, at invocation, not at snapshot time: an entry
                    // cancelled after this snapshot was loaded must not be called.
                    if (entry.gate->retired.load(std::memory_order_acquire)) continue;
                    try {
                        entry.callback(entry.id, data, len, schema, att);
                    } catch (...) {
                        // Spec §5.3: a callback must not throw, because a
                        // provider invokes it from a transport thread where an
                        // escaping exception is a process termination rather than
                        // an unwind. One misbehaving subscriber must not abort
                        // the fan-out for everyone after it in the list either.
                        // Contained here rather than translated: this frame has
                        // no channel to report on, and inventing one would be a
                        // new contract.
                        //
                        // COUNTED, though, and that is not decoration. A
                        // contained failure that increments nothing is
                        // indistinguishable from success at every observation
                        // point a caller has -- the silent wrong answer this
                        // round exists to remove. This Subscriber's own
                        // AbsorbedCallbackFailures() is the observable;
                        // DeliveryChannel::AbsorbedTotal() is its counterpart
                        // one tier down, which stays process-wide because the
                        // clause that reads it can only see a ProviderSubject.
                        absorbed_here->fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });

        lock.lock();

        // The topic may have been removed while we were unlocked.
        auto topic_it = topics.find(key);
        if (topic_it == topics.end()) {
            return result.schema;
        }

        TopicState& current = topic_it->second;
        current.provider_subscribed = true;
        // Cache the provider's schema arrival so fan-out subscribers share it.
        current.schema_arrival = result.schema;
        return current.schema_arrival;
    }
};

uint64_t Subscriber::AbsorbedCallbackFailures() const noexcept {
    return impl_->absorbed->load(std::memory_order_relaxed);
}

Subscriber::Subscriber(std::shared_ptr<PubSubProvider> provider) : impl_(std::make_unique<Impl>()) {
    if (!provider) {
        throw PubSubError(PubSubStatus::kInvalidArgument, "Subscriber: provider must not be null");
    }
    impl_->provider = std::move(provider);
}

Subscriber::~Subscriber() {
    std::vector<std::vector<std::string>> to_unsub;
    std::vector<std::vector<std::string>> schema_to_release;
    // Ids as well as gates: a drain that lands in the carve-out defers its
    // release by id, exactly as Unsubscribe's does.
    std::vector<std::pair<uint64_t, std::shared_ptr<Gate>>> gates;
    {
        std::lock_guard lock(impl_->mu);
        for (auto& [key, ts] : impl_->topics) {
            for (const Impl::Entry& entry : *ts.fanout->entries.load()) {
                gates.emplace_back(entry.id, entry.gate);
                impl_->retirements->Publish(entry.id, entry.gate);
            }
            // Publish an empty list so a delivery that starts from here on finds
            // nothing; the gates below cover one that already holds a snapshot.
            Impl::RewriteEntries(ts, [](std::vector<Impl::Entry>& v) { v.clear(); });
        }
        impl_->subscription_topic.clear();
    }
    // Retire and drain BEFORE entering the provider, and with no lock held — see
    // the lock-order note on Impl::RetireAndDrain. A subscription mid-retirement
    // on ANOTHER thread is not in this list — §6 clause 5 makes destruction
    // require quiescence, so a cancel racing the destructor is already outside
    // the contract.
    for (const auto& [id, gate] : gates) {
        if (!impl_->RetireAndDrain(id, gate, /*owns_retirement=*/true)) {
            impl_->retirements->Release(id);
        }
    }
    // The provider transition is decided after the drain, in one critical
    // section, for the reason Unsubscribe does the same: `provider_subscribed`
    // is never left false across an unbounded wait.
    //
    // **This does NOT ask the door question `Unsubscribe` asks, and the
    // asymmetry is deliberate.** Unsubscribe skips the provider teardown when it
    // is inside a delivery, which is exactly what leaves `provider_subscribed`
    // true for this loop to find -- so the tolerated path feeds the terminating
    // one. That is the designed answer, not an oversight: §6 clause 5 forbids
    // destroying a seam object over a provider with a delivery in flight on this
    // thread, and owner ruling 2026-09-05 chose a named stop over a silent leak
    // for a forbidden act. Published in subscriber.hpp, where an application
    // author reads it.
    //
    // The schema watches collected alongside it DO ask the door question, and
    // the asymmetry is the other way round for a reason that is not a
    // preference: a watch has no delivery frame of its own, so skipping its
    // release strands a transport resource and nothing else, whereas the
    // provider's door would answer this forward with kReentrantCall and that
    // status leaving a `noexcept` destructor is the stop above. One named stop
    // per violation is the ruling; a second one reached through a path with a
    // harmless alternative would be noise. Published in subscriber.hpp.
    {
        std::lock_guard lock(impl_->mu);
        const bool may_enter_provider = !internal::InsideDeliveryOn(impl_->provider.get());
        for (auto& [key, ts] : impl_->topics) {
            if (ts.schema_watches > 0 && may_enter_provider) {
                schema_to_release.push_back(ts.segments);
                ts.schema_watches = 0;
            }
            if (ts.provider_subscribed) {
                to_unsub.push_back(ts.segments);
                ts.provider_subscribed = false;
            }
        }
    }
    // Before the data teardown: a provider that shares one schema channel
    // between the two sides then releases the watch while the data reader is
    // still up and tears the channel down once, instead of re-opening a
    // schema-only reader it is about to destroy.
    for (const auto& segs : schema_to_release) {
        try {
            impl_->provider->UnsubscribeSchema(segs);
        } catch (...) {
            // Nothing to recover during destruction, and no kReentrantCall to
            // rethrow: that question was asked and answered above.
        }
    }
    for (const auto& segs : to_unsub) {
        try {
            impl_->provider->Unsubscribe(segs);
        } catch (const PubSubError& e) {
            // §6 clause 5, as widened by owner ruling 2026-09-05: destroying any
            // seam object over a provider instance requires that no delivery on
            // that instance is in flight on this thread. A handler on subscriber X
            // destroying subscriber Y over the same provider violates it, and the
            // provider's door says so with kReentrantCall.
            //
            // Rethrown, out of a (noexcept) destructor, so the program STOPS
            // naming the violation — the ruling's "a stated, named error instead
            // of a silent one". Swallowing it would leak the transport
            // subscription and Y's Impl with no signal and no bound, which is the
            // silent failure the owner rejected; the message spells
            // `kReentrantCall` in text because the default terminate handler
            // prints what() and not the number.
            //
            // Every OTHER teardown failure stays swallowed: TranslateSeamFailure
            // is total (its last arm is catch(...) -> kInternal), so a
            // PubSubError is all a provider's Unsubscribe can produce, and there
            // is nothing to recover during destruction.
            if (e.status() == PubSubStatus::kReentrantCall) throw;
        } catch (...) {
        }
    }
}

Subscriber::SubscribeResult Subscriber::Subscribe(const std::vector<std::string>& segments,
                                                  SubscribeCallback cb) {
    std::string key = internal::JoinSegments(segments);
    std::unique_lock lock(impl_->mu);

    auto [it, inserted] = impl_->topics.try_emplace(key);
    if (inserted) {
        it->second.segments = segments;
    }

    uint64_t id = impl_->next_id.fetch_add(1);
    impl_->subscription_topic[id] = key;
    Impl::RewriteEntries(it->second, [&](std::vector<Impl::Entry>& v) {
        v.push_back({id, std::move(cb), std::make_shared<Gate>()});
    });

    SchemaArrival schema;
    try {
        schema = impl_->EnsureProviderSubscription(key, it->second, lock);
    } catch (...) {
        // Provider subscription failed — roll back the local subscription record so callers can
        // retry without leaving dangling state behind. EnsureProviderSubscription drops the lock
        // before calling the provider and only retakes it on success, so the lock may or may not be
        // held here; retake it if not, and re-find the topic rather than reusing `it`.
        if (!lock.owns_lock()) lock.lock();
        impl_->subscription_topic.erase(id);
        std::shared_ptr<Gate> gate;
        auto topic_it = impl_->topics.find(key);
        if (topic_it != impl_->topics.end()) {
            Impl::RewriteEntries(topic_it->second, [&](std::vector<Impl::Entry>& v) {
                for (const Impl::Entry& e : v) {
                    if (e.id == id) {
                        gate = e.gate;
                        break;
                    }
                }
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [id](const Impl::Entry& e) { return e.id == id; }),
                        v.end());
            });
        }
        // Retire the rolled-back entry too, outside `mu` and before rethrowing: a
        // provider that delivered once and then failed must not reach a callback
        // whose Subscribe never returned. Published in `retirements` like any
        // other retirement, so a concurrent cancel of this id waits for the same
        // drain rather than finding neither map.
        if (gate) impl_->retirements->Publish(id, gate);
        lock.unlock();
        if (gate && !impl_->RetireAndDrain(id, gate, /*owns_retirement=*/true)) {
            impl_->retirements->Release(id);
        }
        throw;
    }
    return {id, std::move(schema)};
}

void Subscriber::Unsubscribe(uint64_t subscription_id) {
    std::string key;
    std::shared_ptr<Gate> gate;
    bool ours = false;
    {
        std::lock_guard lock(impl_->mu);

        auto it = impl_->subscription_topic.find(subscription_id);
        if (it == impl_->subscription_topic.end()) {
            gate = impl_->retirements->Find(subscription_id);
            if (!gate) {
                // Unknown, or already fully cancelled: accepted, and does nothing
                // (owner ruling 2026-09-04). A foreign-runtime finaliser cancels
                // unconditionally during teardown and cannot let an error escape,
                // and the provider tier below has said the same of an unknown
                // topic since it was written. The cost is deliberate and
                // published: a mistyped id is ignored rather than reported.
                return;
            }
            // Being cancelled RIGHT NOW by another thread. Not the same thing,
            // and not a no-op (owner ruling 2026-09-04): wait for the same drain
            // the winner is performing, so this caller may free its handler state
            // on return exactly like the winner. Returning early here was a
            // second, unpublished exception to the frozen promise — reachable
            // only under a race, which is the hardest kind to discover.
        } else {
            ours = true;
            key = it->second;
            impl_->subscription_topic.erase(it);

            auto topic_it = impl_->topics.find(key);
            if (topic_it != impl_->topics.end()) {
                Impl::RewriteEntries(topic_it->second, [&](std::vector<Impl::Entry>& v) {
                    // Capture the gate BEFORE remove_if, which leaves the tail
                    // moved-from and would hand back a null gate.
                    for (const Impl::Entry& e : v) {
                        if (e.id == subscription_id) {
                            gate = e.gate;
                            break;
                        }
                    }
                    v.erase(std::remove_if(v.begin(), v.end(),
                                           [subscription_id](const Impl::Entry& e) {
                                               return e.id == subscription_id;
                                           }),
                            v.end());
                });
            }
            // Published for the duration of the drain, so a duplicate cancel
            // arriving meanwhile can find this gate and wait on it too. Under
            // `mu`, in the same critical section that removed it from the live
            // map, so "live", "retiring" and "gone" have no window between them.
            if (gate) impl_->retirements->Publish(subscription_id, gate);
        }
    }

    // Outside every lock — see the lock-order note on Impl::RetireAndDrain.
    bool deferred = false;
    if (gate) deferred = impl_->RetireAndDrain(subscription_id, gate, ours);

    // A duplicate cancel has now waited for the winner's drain; the winner owns
    // everything below.
    if (!ours) return;

    // The provider-level transition is decided AFTER the drain, in one critical
    // section, and never published across it. Clearing `provider_subscribed`
    // before draining left the topic looking unsubscribed for the whole duration
    // of an in-flight handler: a Subscribe landing in that window registered a
    // fresh provider subscription which this call then tore down, leaving the
    // newcomer silently receiving nothing forever. Re-checking here means the
    // newcomer's entry is simply visible, and the teardown does not happen.
    std::vector<std::string> segments_to_unsub;
    {
        std::lock_guard lock(impl_->mu);
        // Unless the drain landed in the carve-out and skipped its barrier: the
        // id then stays published until this thread's delivery frame returns, so
        // a cancel from any other thread still finds the gate and waits the
        // handler out instead of taking the no-op branch.
        if (!deferred) impl_->retirements->Release(subscription_id);

        auto topic_it = impl_->topics.find(key);
        if (topic_it != impl_->topics.end() && topic_it->second.fanout->entries.load()->empty() &&
            topic_it->second.provider_subscribed &&
            // A handler cancelling its own last subscription would otherwise
            // reach provider->Unsubscribe from inside that provider's own
            // delivery frame, where the provider's door refuses it with
            // kReentrantCall (spec §6 clause 6). Fletcher's own cancel path never
            // raises that refusal: it asks the same question at its own door and
            // skips the transport-level teardown instead.
            //
            // Checked HERE, inside the critical section and BEFORE the flip
            // below, not after: skipping after clearing `provider_subscribed`
            // would make the next Subscribe on this topic register a SECOND
            // provider subscription over the one still live.
            //
            // The residue is published in subscriber.hpp beside A4's carve-out
            // (owner ruling 2026-09-05): the transport subscription stays open and
            // quiet until this Subscriber is destroyed or the topic is subscribed
            // again. No callback runs either way — the fan-out is empty and every
            // gate is retired — so nothing is unsafe; a resource is simply held
            // longer than a reader might expect.
            !internal::InsideDeliveryOn(impl_->provider.get())) {
            segments_to_unsub = topic_it->second.segments;
            topic_it->second.provider_subscribed = false;
        }
    }

    if (!segments_to_unsub.empty()) {
        impl_->provider->Unsubscribe(segments_to_unsub);
    }
}

SchemaArrival Subscriber::SubscribeSchema(const std::vector<std::string>& segments) {
    // ── The door, before `mu` and before the wait ──────────────────
    //
    // Same ordering argument as EnsureProviderSubscription's, and it applies
    // here without the fast path that softens it there: this call has nothing to
    // serve from the cache before a first watch exists, so it must be able to
    // enter the provider, and from inside that provider's own delivery frame it
    // cannot be. Waiting on `schema_watch_in_progress` instead would be waiting
    // for a flag only a thread inside the provider can clear — which, on a
    // provider that dispatches under its instance mutex, is a thread this
    // delivery is blocking. Refused at THIS tier so a provider that forgot its
    // own door still answers uniformly, and so the refusal is not buried under
    // a rollback — the two reasons EnsureProviderSubscription spells out.
    internal::RefuseIfInsideDeliveryOn(impl_->provider.get(), "Subscriber::SubscribeSchema");

    std::string key = internal::JoinSegments(segments);
    std::unique_lock lock(impl_->mu);

    auto [it, inserted] = impl_->topics.try_emplace(key);
    if (inserted) {
        it->second.segments = segments;
    }
    Impl::TopicState& ts = it->second;

    // `ts` stays valid across the wait and the unlock below: nothing ever erases
    // from `topics`, and unordered_map nodes are stable.
    impl_->provider_cv.wait(lock, [&ts] { return !ts.schema_watch_in_progress; });

    // Read AFTER that wait, never before it: the thread we waited out is the one
    // that stored the arrival, and the count it incremented on the way in was
    // already visible while the arrival was not.
    if (ts.schema_watches > 0) {
        ++ts.schema_watches;
        return ts.schema_watch_arrival;
    }

    ts.schema_watch_in_progress = true;
    ++ts.schema_watches;
    lock.unlock();

    SchemaArrival arrival;
    try {
        // Never with `mu` held — the lock-order note on Impl::RetireAndDrain.
        arrival = impl_->provider->SubscribeSchema(segments);
    } catch (...) {
        // Roll the count back so a later call retries the provider rather than
        // handing out an arrival nothing opened, and wake the waiters either way
        // or every later SubscribeSchema on this topic waits forever.
        lock.lock();
        --ts.schema_watches;
        ts.schema_watch_in_progress = false;
        impl_->provider_cv.notify_all();
        throw;
    }

    lock.lock();
    ts.schema_watch_arrival = arrival;
    ts.schema_watch_in_progress = false;
    impl_->provider_cv.notify_all();
    return arrival;
}

void Subscriber::UnsubscribeSchema(const std::vector<std::string>& segments) {
    std::unique_lock lock(impl_->mu);

    auto it = impl_->topics.find(internal::JoinSegments(segments));
    // Nothing counted here: not this Subscriber's watch to release. A no-op, not
    // an error, exactly as cancelling an unknown id is.
    if (it == impl_->topics.end() || it->second.schema_watches == 0) return;
    Impl::TopicState& ts = it->second;
    // Not serialised against a first SubscribeSchema still inside the provider
    // on another thread — deliberately. Reaching this line while that call is in
    // flight means releasing a watch whose SubscribeSchema has not returned,
    // which no caller is legitimately in a position to do, and waiting for
    // `schema_watch_in_progress` here would be the wait the door on the other
    // side exists to prevent: this tier has no door of its own to raise first.
    if (--ts.schema_watches > 0) return;

    std::vector<std::string> segments_to_release = ts.segments;
    lock.unlock();

    try {
        impl_->provider->UnsubscribeSchema(segments_to_release);
    } catch (...) {
        // The provider still holds the watch — its door refuses a call from
        // inside a delivery like every other method's, and this tier has no
        // carve-out to offer, a watch having no delivery frame to wait for. So
        // restore the count and let the refusal reach the caller: the watch stays
        // this Subscriber's, releasable later or by ~Subscriber, instead of
        // becoming a resource neither tier believes it holds.
        lock.lock();
        ++ts.schema_watches;
        throw;
    }
}

}  // namespace fletcher
