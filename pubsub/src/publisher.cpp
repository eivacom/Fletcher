// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/publisher.hpp"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "fletcher/pubsub/internal/schema_conflict.hpp"
#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

struct Publisher::Impl {
    std::shared_ptr<PubSubProvider> provider;
    mutable std::mutex mu;
    // The declared schema per topic, as internal::DeclaredSchema — the same type, and the same
    // comparison, the in-process provider uses for its own conflict check.
    std::unordered_map<std::string, internal::DeclaredSchema> topics;
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

    // Re-declaring an existing topic is idempotent for an identical schema — which lets several
    // publishers share one topic (fan-in) — while a different schema for the same topic is a
    // genuine conflict that must not be silently accepted.
    //
    // Encode before taking the lock, so the locked section is a byte compare rather than two IPC
    // encodes that every concurrent CreateTopic queues behind. A first declaration pays an encode
    // where it previously only deep-copied; see the FastDDS provider README, "Measured decisions".
    internal::DeclaredSchema incoming = internal::DeclaredSchema::Encode(schema.get());

    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->topics.find(key);
        if (it != impl_->topics.end()) {
            if (incoming.ConflictsWith(it->second)) {
                throw std::runtime_error(
                    "Publisher: topic already declared with a conflicting schema: " + key);
            }
            return;  // identical (or non-comparable) re-declaration — no-op
        }
        impl_->topics.emplace(key, std::move(incoming));
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
                        const PubSubProvider::RowEncoder& encoder, const Attachments& attachments) {
    impl_->provider->Publish(segments, encoder, attachments);
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
