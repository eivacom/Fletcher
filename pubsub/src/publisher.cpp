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

Publisher::Publisher(std::shared_ptr<PubSubProvider> provider)
    : impl_(std::make_unique<Impl>()) {
    if (!provider) {
        throw std::invalid_argument("Publisher: provider must not be null");
    }
    impl_->provider = std::move(provider);
}

Publisher::~Publisher() = default;

void Publisher::CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) {
    std::string key = internal::JoinSegments(segments);
    {
        std::lock_guard lock(impl_->mu);
        if (impl_->topics.count(key)) {
            throw std::runtime_error("Publisher: topic already exists: " + key);
        }
    }

    OwnedSchema local_copy;
    if (schema) {
        local_copy = OwnedSchema::DeepCopy(schema.get());
    }

    impl_->provider->CreateTopic(segments, std::move(schema));

    {
        std::lock_guard lock(impl_->mu);
        Impl::TopicState ts;
        ts.segments = segments;
        ts.schema = std::move(local_copy);
        impl_->topics[key] = std::move(ts);
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

bool Publisher::HasTopic(const std::vector<std::string>& segments) const {
    std::string key = internal::JoinSegments(segments);
    std::lock_guard lock(impl_->mu);
    return impl_->topics.count(key) > 0;
}

}  // namespace fletcher
