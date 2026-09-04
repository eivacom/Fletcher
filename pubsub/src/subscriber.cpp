// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/subscriber.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
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
// The storage is thread-local, but the SCOPE is per Subscriber (owner ruling
// 2026-09-04): the stack holds one identity token per delivery frame, and the
// predicate asks for a specific token. A handler on subscriber X cancelling on
// subscriber Y therefore does NOT skip Y's barrier — Y's caller reads the
// published sentence, believes the wait happened, and is right. A file-local
// depth counter would have made that sentence false in the unsafe direction.
//
// The token is compared by address and NEVER dereferenced: the provider callback
// keeps it alive by shared_ptr, so it outlives the Subscriber it identifies.
thread_local std::vector<const void*> g_delivery_stack;

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

// Releases owed by this thread's delivery frames — see DeliveryScope.
struct DeferredRelease {
    std::shared_ptr<Retirements> retirements;
    uint64_t id;
};
thread_local std::vector<DeferredRelease> g_deferred_releases;

class DeliveryScope {
   public:
    explicit DeliveryScope(const void* token) { g_delivery_stack.push_back(token); }

    // When a cancellation issued from inside a delivery skips its barrier, the
    // id must STAY published until this thread's outermost delivery frame
    // returns — otherwise the winner un-publishes the drain the moment it skips,
    // and a cancel of the same id from any OTHER thread finds neither map, takes
    // the no-op branch, and returns while that callback is still running. That
    // caller is not covered by the published carve-out, so leaving it there gave
    // the frozen promise a second exception.
    //
    // Swept at depth 0, not per frame: while any frame of this thread is still
    // on the stack, a gate it holds is still held. Costs nothing when the list is
    // empty, which is every delivery that did not cancel anything.
    ~DeliveryScope() {
        g_delivery_stack.pop_back();
        if (!g_delivery_stack.empty() || g_deferred_releases.empty()) return;
        std::vector<DeferredRelease> due;
        due.swap(g_deferred_releases);
        for (const DeferredRelease& release : due) {
            release.retirements->Release(release.id);
        }
    }

    DeliveryScope(const DeliveryScope&) = delete;
    DeliveryScope& operator=(const DeliveryScope&) = delete;
};

bool InsideDeliveryOn(const void* token) {
    return std::find(g_delivery_stack.begin(), g_delivery_stack.end(), token) !=
           g_delivery_stack.end();
}

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
    };

    std::shared_ptr<PubSubProvider> provider;

    std::shared_ptr<Identity> identity = std::make_shared<Identity>();

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
            g_deferred_releases.push_back(DeferredRelease{retirements, id});
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
        // `ts` stays valid across the wait and across the unlock below: nothing
        // ever erases from `topics`, and unordered_map nodes are stable.
        provider_cv.wait(lock, [&ts] { return !ts.provider_subscribe_in_progress; });

        if (ts.provider_subscribed) {
            return ts.schema_arrival;
        }

        std::vector<std::string> segments = ts.segments;
        FanoutPtr fanout = ts.fanout;
        std::shared_ptr<Identity> token = identity;

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
            segments, [fanout, token](const uint8_t* data, size_t len, const SharedSchema& schema,
                                      const Attachments& att) {
                EntryList entries = fanout->entries.load();
                // One push/pop per sample, not per entry: it marks the whole
                // fan-out frame, so a handler cancelling ANY subscription on this
                // Subscriber skips the barrier rather than blocking on a gate.
                DeliveryScope scope(token.get());
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

Subscriber::Subscriber(std::shared_ptr<PubSubProvider> provider) : impl_(std::make_unique<Impl>()) {
    if (!provider) {
        throw PubSubError(PubSubStatus::kInvalidArgument, "Subscriber: provider must not be null");
    }
    impl_->provider = std::move(provider);
}

Subscriber::~Subscriber() {
    std::vector<std::vector<std::string>> to_unsub;
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
    {
        std::lock_guard lock(impl_->mu);
        for (auto& [key, ts] : impl_->topics) {
            if (ts.provider_subscribed) {
                to_unsub.push_back(ts.segments);
                ts.provider_subscribed = false;
            }
        }
    }
    for (const auto& segs : to_unsub) {
        try {
            impl_->provider->Unsubscribe(segs);
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
            topic_it->second.provider_subscribed) {
            segments_to_unsub = topic_it->second.segments;
            topic_it->second.provider_subscribed = false;
        }
    }

    if (!segments_to_unsub.empty()) {
        impl_->provider->Unsubscribe(segments_to_unsub);
    }
}

}  // namespace fletcher
