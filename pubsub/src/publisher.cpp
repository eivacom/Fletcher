// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/publisher.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "fletcher/pubsub/internal/segments.hpp"
#include "fletcher/pubsub/schema_ipc.hpp"

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
    std::vector<uint8_t> new_ipc =
        schema ? SerializeSchemaIpc(schema.get()) : std::vector<uint8_t>{};

    // Atomically claim the topic key under the lock. Re-declaring an existing
    // topic is idempotent for an identical schema — which lets several
    // publishers share one topic (fan-in) — while a different schema for the
    // same topic is a genuine conflict that must not be silently accepted.
    // Claiming with the schema lets a concurrent duplicate compare against it.
    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->topics.find(key);
        if (it != impl_->topics.end()) {
            std::vector<uint8_t> existing = it->second.schema
                                                ? SerializeSchemaIpc(it->second.schema.get())
                                                : std::vector<uint8_t>{};
            if (new_ipc != existing) {
                throw std::runtime_error(
                    "Publisher: topic already declared with a conflicting schema: " + key);
            }
            return;  // identical re-declaration — no-op
        }
        Impl::TopicState ts;
        ts.segments = segments;
        if (schema) {
            ts.schema = OwnedSchema::DeepCopy(schema.get());
        }
        impl_->topics.emplace(key, std::move(ts));
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
