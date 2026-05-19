// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "pubsub/driver.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace fletcher {

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

namespace {

std::string JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) return {};
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// Impl
// -----------------------------------------------------------------------

struct Driver::Impl {
    struct TopicState {
        std::vector<std::string> segments;
        OwnedSchema              schema;
        bool                     provider_subscribed = false;
    };

    struct Subscription {
        std::string      topic_key;
        SubscribeCallback callback;
    };

    std::shared_ptr<PubSub> provider;

    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState>    topics;
    std::unordered_map<uint64_t, Subscription>     subscriptions;
    std::atomic<uint64_t> next_id{1};

    // Called with mu held.  Releases the lock while calling into the
    // provider to avoid deadlock if the provider calls back synchronously.
    OwnedSchema EnsureProviderSubscription(
        const std::string& key, TopicState& ts,
        std::unique_lock<std::mutex>& lock,
        std::any config = {}) {
        if (ts.provider_subscribed) {
            if (ts.schema)
                return OwnedSchema::DeepCopy(ts.schema.get());
            return {};
        }

        // Capture what we need before releasing the lock.
        auto segments = ts.segments;
        OwnedSchema cached_schema;
        if (ts.schema)
            cached_schema = OwnedSchema::DeepCopy(ts.schema.get());

        lock.unlock();

        auto result = provider->Subscribe(
            segments, [this, key](const uint8_t* data, size_t len,
                                      SharedSchema schema,
                                      Attachments att) {
                std::vector<SubscribeCallback> cbs;
                {
                    std::lock_guard lk(mu);
                    for (auto& [id, sub] : subscriptions) {
                        if (sub.topic_key == key)
                            cbs.push_back(sub.callback);
                    }
                }
                for (auto& cb : cbs)
                    cb(data, len, schema, att);
            }, std::move(config));

        lock.lock();

        // The topic may have been removed while we were unlocked.
        auto topic_it = topics.find(key);
        if (topic_it == topics.end()) {
            if (result.schema)
                return OwnedSchema::DeepCopy(result.schema.get());
            return std::move(cached_schema);
        }

        TopicState& current = topic_it->second;
        current.provider_subscribed = true;
        OwnedSchema ret;
        if (result.schema) {
            if (!current.schema)
                current.schema = OwnedSchema::DeepCopy(result.schema.get());
            ret = OwnedSchema::DeepCopy(result.schema.get());
        } else if (current.schema) {
            ret = OwnedSchema::DeepCopy(current.schema.get());
        }
        return ret;
    }
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

Driver::Driver(std::shared_ptr<PubSub> provider)
    : impl_(std::make_unique<Impl>()) {
    if (!provider)
        throw std::invalid_argument("Driver: provider must not be null");
    impl_->provider = std::move(provider);
}

Driver::~Driver() {
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
    for (auto& segs : to_unsub)
        impl_->provider->Unsubscribe(segs);
}

// -----------------------------------------------------------------------
// Topic management
// -----------------------------------------------------------------------

void Driver::CreateTopic(const std::vector<std::string>& segments,
                         OwnedSchema schema,
                         std::any config) {
    std::string key = JoinSegments(segments);
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->topics.count(key))
            throw std::runtime_error("Driver: topic already exists: " + key);
    }

    // Deep-copy for local storage before moving into provider.
    OwnedSchema local_copy;
    if (schema)
        local_copy = OwnedSchema::DeepCopy(schema.get());

    impl_->provider->CreateTopic(segments, std::move(schema), std::move(config));

    {
        std::lock_guard lock(impl_->mu);
        Impl::TopicState ts;
        ts.segments = segments;
        ts.schema   = std::move(local_copy);
        impl_->topics[key] = std::move(ts);
    }
}

void Driver::Publish(const std::vector<std::string>& segments,
                     PubSub::RowEncoder encoder,
                     const Attachments& attachments) {
    impl_->provider->Publish(segments, std::move(encoder), attachments);
}

// -----------------------------------------------------------------------
// Subscription
// -----------------------------------------------------------------------

Driver::SubscribeResult Driver::Subscribe(
    const std::vector<std::string>& segments, SubscribeCallback cb,
    std::any config) {
    std::string key = JoinSegments(segments);
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
    auto schema = impl_->EnsureProviderSubscription(key, it->second, lock,
                                                     std::move(config));
    return {id, std::move(schema)};
}

void Driver::Unsubscribe(uint64_t subscription_id) {
    std::vector<std::string> segments_to_unsub;
    {
        std::lock_guard lock(impl_->mu);

        auto it = impl_->subscriptions.find(subscription_id);
        if (it == impl_->subscriptions.end())
            throw std::runtime_error("Driver: unknown subscription ID");

        std::string key = it->second.topic_key;
        impl_->subscriptions.erase(it);

        bool any_remaining = false;
        for (auto& [id, sub] : impl_->subscriptions) {
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

    if (!segments_to_unsub.empty())
        impl_->provider->Unsubscribe(segments_to_unsub);
}

// -----------------------------------------------------------------------
// Introspection
// -----------------------------------------------------------------------

std::vector<std::string> Driver::ListTopics() const {
    std::lock_guard lock(impl_->mu);
    std::vector<std::string> result;
    result.reserve(impl_->topics.size());
    for (auto& [key, _] : impl_->topics)
        result.push_back(key);
    return result;
}

bool Driver::HasTopic(const std::vector<std::string>& segments) const {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);
    return impl_->topics.count(key) > 0;
}

}  // namespace fletcher
