// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/subscriber.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

struct Subscriber::Impl {
    struct Entry {
        uint64_t id;
        SubscribeCallback callback;
    };

    // Immutable snapshot of a topic's subscribers, so delivery copies one shared_ptr instead of
    // scanning every subscription in the process and copying a std::function per match. Rebuilt on
    // Subscribe/Unsubscribe (rare); read on every sample (hot).
    using EntryList = std::shared_ptr<const std::vector<Entry>>;

    struct TopicState {
        std::vector<std::string> segments;
        EntryList subscribers = std::make_shared<const std::vector<Entry>>();
        // The provider's schema future, cached so fan-out subscribers to the
        // same topic all share it (shared_future is copyable).
        std::shared_future<SharedSchema> schema_future;
        bool provider_subscribed = false;
    };

    std::shared_ptr<PubSubProvider> provider;

    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
    // Only maps an id to its topic, so Unsubscribe can find the list to rebuild. The callback
    // itself lives in that topic's EntryList.
    std::unordered_map<uint64_t, std::string> subscription_topic;
    std::atomic<uint64_t> next_id{1};

    // Copy-on-write mutation of one topic's subscriber list. Called with mu held. Never mutates a
    // published list, so a delivery already iterating an older snapshot stays valid.
    template <typename Mutate>
    static void RewriteEntries(TopicState& ts, Mutate mutate) {
        auto next = std::make_shared<std::vector<Entry>>(*ts.subscribers);
        mutate(*next);
        ts.subscribers = std::move(next);
    }

    // Called with mu held. Releases the lock while calling into the
    // provider to avoid deadlock if the provider calls back synchronously.
    std::shared_future<SharedSchema> EnsureProviderSubscription(
        const std::string& key, TopicState& ts, std::unique_lock<std::mutex>& lock) {
        if (ts.provider_subscribed) {
            return ts.schema_future;
        }

        std::vector<std::string> segments = ts.segments;

        lock.unlock();

        SubscriptionResult result = provider->Subscribe(
            segments,
            [this, key](const uint8_t* data, size_t len, const SharedSchema& schema,
                        const Attachments& att) {
                EntryList entries;
                {
                    std::lock_guard lk(mu);
                    auto it = topics.find(key);
                    if (it == topics.end()) return;
                    entries = it->second.subscribers;
                }
                if (entries->empty()) return;

                // Copy the schema and attachments to all but the last callback; move into the last
                // one to avoid an unnecessary copy when the attachments map is large.
                for (size_t i = 0; i + 1 < entries->size(); ++i) {
                    const Entry& entry = (*entries)[i];
                    entry.callback(entry.id, data, len, schema, att);
                }
                const Entry& last = entries->back();
                last.callback(last.id, data, len, std::move(schema), std::move(att));
            });

        lock.lock();

        // The topic may have been removed while we were unlocked.
        auto topic_it = topics.find(key);
        if (topic_it == topics.end()) {
            return result.schema;
        }

        TopicState& current = topic_it->second;
        current.provider_subscribed = true;
        // Cache the provider's schema future so fan-out subscribers share it.
        current.schema_future = result.schema;
        return current.schema_future;
    }
};

Subscriber::Subscriber(std::shared_ptr<PubSubProvider> provider) : impl_(std::make_unique<Impl>()) {
    if (!provider) {
        throw std::invalid_argument("Subscriber: provider must not be null");
    }
    impl_->provider = std::move(provider);
}

Subscriber::~Subscriber() {
    std::vector<std::vector<std::string>> to_unsub;
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

    auto it = impl_->topics.find(key);
    if (it == impl_->topics.end()) {
        Impl::TopicState ts;
        ts.segments = segments;
        impl_->topics[key] = std::move(ts);
        it = impl_->topics.find(key);
    }

    uint64_t id = impl_->next_id.fetch_add(1);
    impl_->subscription_topic[id] = key;
    Impl::RewriteEntries(it->second,
                         [&](std::vector<Impl::Entry>& v) { v.push_back({id, std::move(cb)}); });

    std::shared_future<SharedSchema> schema;
    try {
        schema = impl_->EnsureProviderSubscription(key, it->second, lock);
    } catch (...) {
        // Provider subscription failed — roll back the local subscription record so callers can
        // retry without leaving dangling state behind. EnsureProviderSubscription drops the lock
        // before calling the provider and only retakes it on success, so the lock may or may not be
        // held here; retake it if not, and re-find the topic rather than reusing `it`.
        if (!lock.owns_lock()) lock.lock();
        impl_->subscription_topic.erase(id);
        auto topic_it = impl_->topics.find(key);
        if (topic_it != impl_->topics.end()) {
            Impl::RewriteEntries(topic_it->second, [&](std::vector<Impl::Entry>& v) {
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [id](const Impl::Entry& e) { return e.id == id; }),
                        v.end());
            });
        }
        throw;
    }
    return {id, std::move(schema)};
}

void Subscriber::Unsubscribe(uint64_t subscription_id) {
    std::vector<std::string> segments_to_unsub;
    {
        std::lock_guard lock(impl_->mu);

        auto it = impl_->subscription_topic.find(subscription_id);
        if (it == impl_->subscription_topic.end()) {
            throw std::runtime_error("Subscriber: unknown subscription ID");
        }

        const std::string key = it->second;
        impl_->subscription_topic.erase(it);

        auto topic_it = impl_->topics.find(key);
        if (topic_it != impl_->topics.end()) {
            Impl::RewriteEntries(topic_it->second, [&](std::vector<Impl::Entry>& v) {
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [subscription_id](const Impl::Entry& e) {
                                           return e.id == subscription_id;
                                       }),
                        v.end());
            });

            if (topic_it->second.subscribers->empty() && topic_it->second.provider_subscribed) {
                segments_to_unsub = topic_it->second.segments;
                topic_it->second.provider_subscribed = false;
            }
        }
    }

    if (!segments_to_unsub.empty()) {
        impl_->provider->Unsubscribe(segments_to_unsub);
    }
}

}  // namespace fletcher
