// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/subscriber.hpp"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

struct Subscriber::Impl {
    struct TopicState {
        std::vector<std::string> segments;
        OwnedSchema schema;
        bool provider_subscribed = false;
    };

    struct Subscription {
        std::string topic_key;
        SubscribeCallback callback;
    };

    std::shared_ptr<PubSubProvider> provider;

    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
    std::unordered_map<uint64_t, Subscription> subscriptions;
    std::atomic<uint64_t> next_id{1};

    // Called with mu held.  Releases the lock while calling into the
    // provider to avoid deadlock if the provider calls back synchronously.
    OwnedSchema EnsureProviderSubscription(const std::string& key, TopicState& ts,
                                           std::unique_lock<std::mutex>& lock) {
        if (ts.provider_subscribed) {
            if (ts.schema) {
                return OwnedSchema::DeepCopy(ts.schema.get());
            }
            return {};
        }

        std::vector<std::string> segments = ts.segments;
        OwnedSchema cached_schema;
        if (ts.schema) {
            cached_schema = OwnedSchema::DeepCopy(ts.schema.get());
        }

        lock.unlock();

        SubscriptionResult result = provider->Subscribe(
            segments,
            [this, key](const uint8_t* data, size_t len, SharedSchema schema, Attachments att) {
                std::vector<SubscribeCallback> cbs;
                {
                    std::lock_guard lk(mu);
                    for (const auto& [id, sub] : subscriptions) {
                        if (sub.topic_key == key) {
                            cbs.push_back(sub.callback);
                        }
                    }
                }
                for (const auto& cb : cbs) {
                    cb(data, len, schema, att);
                }
            });

        lock.lock();

        // The topic may have been removed while we were unlocked.
        auto topic_it = topics.find(key);
        if (topic_it == topics.end()) {
            if (result.schema) {
                return OwnedSchema::DeepCopy(result.schema.get());
            }
            return std::move(cached_schema);
        }

        TopicState& current = topic_it->second;
        current.provider_subscribed = true;
        OwnedSchema ret;
        if (result.schema) {
            if (!current.schema) {
                current.schema = OwnedSchema::DeepCopy(result.schema.get());
            }
            ret = OwnedSchema::DeepCopy(result.schema.get());
        } else if (current.schema) {
            ret = OwnedSchema::DeepCopy(current.schema.get());
        }
        return ret;
    }
};

Subscriber::Subscriber(std::shared_ptr<PubSubProvider> provider)
    : impl_(std::make_unique<Impl>()) {
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
    impl_->subscriptions[id] = Impl::Subscription{key, std::move(cb)};
    OwnedSchema schema = impl_->EnsureProviderSubscription(key, it->second, lock);
    return {id, std::move(schema)};
}

void Subscriber::Unsubscribe(uint64_t subscription_id) {
    std::vector<std::string> segments_to_unsub;
    {
        std::lock_guard lock(impl_->mu);

        auto it = impl_->subscriptions.find(subscription_id);
        if (it == impl_->subscriptions.end()) {
            throw std::runtime_error("Subscriber: unknown subscription ID");
        }

        std::string key = it->second.topic_key;
        impl_->subscriptions.erase(it);

        bool any_remaining = false;
        for (const auto& [id, sub] : impl_->subscriptions) {
            if (sub.topic_key == key) {
                any_remaining = true;
                break;
            }
        }

        if (!any_remaining) {
            auto topic_it = impl_->topics.find(key);
            if (topic_it != impl_->topics.end() && topic_it->second.provider_subscribed) {
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
