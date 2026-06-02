// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/publisher.hpp"

#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

struct Publisher::Impl {
    struct TopicState {
        std::vector<std::string> segments;
        OwnedSchema schema;
    };

    std::shared_ptr<PubSubProvider> provider;
    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
};

Publisher::Publisher(std::shared_ptr<PubSubProvider> provider) : impl_(std::make_unique<Impl>()) {
    if (!provider) {
        throw std::invalid_argument("Publisher: provider must not be null");
    }
    impl_->provider = std::move(provider);
}

Publisher::~Publisher() = default;

void Publisher::CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) {
    std::string key = internal::JoinSegments(segments);

    // Atomically claim the topic key under the lock so two concurrent
    // CreateTopic calls for the same topic cannot both pass the
    // duplicate check and reach provider->CreateTopic.
    {
        std::lock_guard lock(impl_->mu);
        Impl::TopicState ts;
        ts.segments = segments;
        auto [it, inserted] = impl_->topics.try_emplace(key, std::move(ts));
        if (!inserted) {
            throw std::runtime_error("Publisher: topic already exists: " + key);
        }
    }

    OwnedSchema local_copy;
    if (schema) {
        local_copy = OwnedSchema::DeepCopy(schema.get());
    }

    try {
        impl_->provider->CreateTopic(segments, std::move(schema));
    } catch (...) {
        // Provider rejected the topic — roll back the claim so a
        // subsequent retry can succeed.
        std::lock_guard lock(impl_->mu);
        impl_->topics.erase(key);
        throw;
    }

    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->topics.find(key);
        if (it != impl_->topics.end()) {
            it->second.schema = std::move(local_copy);
        }
    }
}

void Publisher::Publish(const std::vector<std::string>& segments,
                        PubSubProvider::RowEncoder encoder, const Attachments& attachments) {
    impl_->provider->Publish(segments, std::move(encoder), attachments);
}

std::vector<std::string> Publisher::ListTopics() const {
    std::lock_guard lock(impl_->mu);
    std::vector<std::string> result;
    result.reserve(impl_->topics.size());
    for (const auto& [key, _] : impl_->topics) {
        result.push_back(key);
    }
    return result;
}

}  // namespace fletcher
