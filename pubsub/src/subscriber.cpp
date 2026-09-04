// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/subscriber.hpp"

#include <algorithm>
#include <atomic>
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

class DeliveryScope {
   public:
    explicit DeliveryScope(const void* token) { g_delivery_stack.push_back(token); }
    ~DeliveryScope() { g_delivery_stack.pop_back(); }
    DeliveryScope(const DeliveryScope&) = delete;
    DeliveryScope& operator=(const DeliveryScope&) = delete;
};

bool InsideDeliveryOn(const void* token) {
    return std::find(g_delivery_stack.begin(), g_delivery_stack.end(), token) !=
           g_delivery_stack.end();
}

}  // namespace

struct Subscriber::Impl {
    // One per subscription. `retired` is stored BEFORE the barrier is taken and
    // read under `mu` at invocation, which is what makes the two interleavings
    // total: a delivery that read `retired == false` under the gate is waited
    // for, and one that had not yet taken the gate reads `true` and skips.
    // "Retired but being invoked" is not a representable state — that is the
    // memory-safety property, not a test.
    //
    // NOT recursive, deliberately: a recursive gate would let a provider that
    // re-entered delivery for one subscription on one thread proceed silently,
    // which would make provider.hpp's "one callback at a time" unfalsifiable and
    // would pre-empt the typed re-entrancy refusal that belongs to a separate
    // item. With a plain mutex that violation deadlocks loudly under the
    // conformance suite's ctest TIMEOUT. The self-cancellation case a recursive
    // gate used to cover is covered by DeliveryScope above instead.
    struct Gate {
        std::mutex mu;
        std::atomic<bool> retired{false};
    };

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
    };

    std::shared_ptr<PubSubProvider> provider;

    std::shared_ptr<Identity> identity = std::make_shared<Identity>();

    // Retire a subscription and wait out any invocation already inside its gate.
    //
    // The barrier's ONLY job is that wait, and its scope is load-bearing twice
    // over — the lock order is total, `mu` < gate < provider, and both new edges
    // are forbidden rather than handled:
    //  - gate → gate: a thread already inside a delivery on THIS Subscriber
    //    skips the barrier entirely, so two cross-cancelling deliveries have no
    //    cycle to form. That is also the one shape where the caller may not free
    //    callback state on return (published in subscriber.hpp).
    //  - gate → provider: callers must let this function RETURN before entering
    //    the provider. A provider's own Unsubscribe may wait for its in-flight
    //    delivery, and that delivery may at that moment be about to take this
    //    gate. Never hold a gate across a provider call.
    void RetireAndDrain(const std::shared_ptr<Gate>& gate) const {
        gate->retired.store(true, std::memory_order_release);
        if (InsideDeliveryOn(identity.get())) return;
        std::lock_guard<std::mutex> barrier(gate->mu);
    }

    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
    // Only maps an id to its topic, so Unsubscribe can find the list to rebuild. The callback
    // itself lives in that topic's EntryList.
    std::unordered_map<uint64_t, std::string> subscription_topic;
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
        if (ts.provider_subscribed) {
            return ts.schema_arrival;
        }

        std::vector<std::string> segments = ts.segments;
        FanoutPtr fanout = ts.fanout;
        std::shared_ptr<Identity> token = identity;

        lock.unlock();

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
                    entry.callback(entry.id, data, len, schema, att);
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
    std::vector<std::shared_ptr<Impl::Gate>> gates;
    {
        std::lock_guard lock(impl_->mu);
        for (auto& [key, ts] : impl_->topics) {
            for (const Impl::Entry& entry : *ts.fanout->entries.load()) {
                gates.push_back(entry.gate);
            }
            // Publish an empty list so a delivery that starts from here on finds
            // nothing; the gates below cover one that already holds a snapshot.
            Impl::RewriteEntries(ts, [](std::vector<Impl::Entry>& v) { v.clear(); });
            if (ts.provider_subscribed) {
                to_unsub.push_back(ts.segments);
                ts.provider_subscribed = false;
            }
        }
        impl_->subscription_topic.clear();
    }
    // Retire and drain BEFORE entering the provider, and outside `mu` — the same
    // total order Unsubscribe keeps, for the same two reasons.
    for (const std::shared_ptr<Impl::Gate>& gate : gates) {
        impl_->RetireAndDrain(gate);
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
        v.push_back({id, std::move(cb), std::make_shared<Impl::Gate>()});
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
        std::shared_ptr<Impl::Gate> gate;
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
        // whose Subscribe never returned.
        lock.unlock();
        if (gate) impl_->RetireAndDrain(gate);
        throw;
    }
    return {id, std::move(schema)};
}

void Subscriber::Unsubscribe(uint64_t subscription_id) {
    std::vector<std::string> segments_to_unsub;
    std::shared_ptr<Impl::Gate> gate;
    {
        std::lock_guard lock(impl_->mu);

        auto it = impl_->subscription_topic.find(subscription_id);
        if (it == impl_->subscription_topic.end()) {
            // Not live: accepted, and does nothing (spec §7 clause 6, owner
            // ruling 2026-09-04). A foreign-runtime finaliser cancels
            // unconditionally during teardown and cannot let an error escape,
            // and the provider tier below has said the same of an unknown topic
            // since it was written. The cost is deliberate and published: a
            // mistyped id is ignored rather than reported.
            return;
        }

        const std::string key = it->second;
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

            if (topic_it->second.fanout->entries.load()->empty() &&
                topic_it->second.provider_subscribed) {
                segments_to_unsub = topic_it->second.segments;
                topic_it->second.provider_subscribed = false;
            }
        }
    }

    // Outside `mu`, and in a scope that ENDS here — the barrier must not still be
    // held when the provider is entered below. See Impl::RetireAndDrain.
    if (gate) impl_->RetireAndDrain(gate);

    if (!segments_to_unsub.empty()) {
        impl_->provider->Unsubscribe(segments_to_unsub);
    }
}

}  // namespace fletcher
