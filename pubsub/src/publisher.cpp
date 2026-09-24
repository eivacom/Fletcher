// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/publisher.hpp"

#include <cstdint>
#include <fletcher/core/status.hpp>
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
    // The declared schema and the TopicOptions in force for a topic. `schema` uses the same type,
    // and the same comparison, the in-process provider uses for its own conflict check; a
    // CreateTopic call that omits `options` carries a default-constructed (empty) one.
    struct TopicEntry {
        internal::DeclaredSchema schema;
        TopicOptions options;
    };
    std::unordered_map<std::string, TopicEntry> topics;
};

Publisher::Publisher(std::shared_ptr<PubSubProvider> provider) : impl_(std::make_unique<Impl>()) {
    if (!provider) {
        throw PubSubError(PubSubStatus::kInvalidArgument, "Publisher: provider must not be null");
    }
    impl_->provider = std::move(provider);
}

Publisher::~Publisher() = default;

void Publisher::CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema,
                            const TopicOptions& options) {
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
            if (incoming.ConflictsWith(it->second.schema)) {
                // The SAME numbered cause a provider reports for the same fact
                // (spec §5.1). This tier short-circuits before the provider — it
                // is the layer the gateway and PublisherArrow sit on, so it is
                // where an application actually meets a schema conflict, and it
                // must not be the one place the number goes missing.
                throw PubSubError(
                    PubSubStatus::kSchemaConflict,
                    "Publisher: topic already declared with a conflicting schema: " + key);
            }
            // Field-wise: a later call may repeat or omit a field already stored, never change
            // one — and a non-empty field against an EMPTY stored one conflicts too, because the
            // endpoint already exists without it.
            const TopicOptions& stored = it->second.options;
            const bool profile_conflict =
                !options.profile.empty() && options.profile != stored.profile;
            const bool bound_conflict = options.max_payload_bytes != 0 &&
                                        options.max_payload_bytes != stored.max_payload_bytes;
            if (profile_conflict || bound_conflict) {
                throw PubSubError(
                    PubSubStatus::kInvalidArgument,
                    "Publisher: topic already declared with different options: " + key);
            }
            return;  // identical (or non-comparable) re-declaration — no-op
        }
        impl_->topics.emplace(key, Impl::TopicEntry{std::move(incoming), options});
    }

    try {
        // Always the options form; the base delegates.
        impl_->provider->CreateTopicWithOptions(segments, std::move(schema), options);
    } catch (...) {
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
