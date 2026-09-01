// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/in_process_provider.hpp"

#include <fletcher/core/write_buffer.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "fletcher/pubsub/internal/schema_conflict.hpp"
#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

struct InProcessPubSubProvider::Impl {
    struct TopicState {
        SubscribeCallback callback;
        // Null when nobody announced one; the gateway lets the client bring its own.
        SharedSchema schema;
        // Set once a publisher has declared this topic. Absent for a topic a
        // subscriber or a publish created lazily, which is why it is optional
        // rather than a flag beside empty bytes: "declared with no schema" and
        // "never declared" are different states, and only the first can conflict.
        std::optional<internal::DeclaredSchema> declared;
    };

    std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
};

InProcessPubSubProvider::InProcessPubSubProvider() : impl_(std::make_unique<Impl>()) {}

InProcessPubSubProvider::~InProcessPubSubProvider() = default;

void InProcessPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                          OwnedSchema schema) {
    std::string key = internal::JoinSegments(topic_segments);

    // Encode before taking the lock, so the locked section is a byte compare
    // rather than an IPC encode every concurrent CreateTopic queues behind.
    // The SAME comparison Publisher::CreateTopic uses one layer up — deliberately
    // not a second implementation of it.
    internal::DeclaredSchema incoming = internal::DeclaredSchema::Encode(schema.get());

    std::lock_guard lock(impl_->mu);
    auto& slot = impl_->topics[key];

    // Re-declaration is idempotent for an identical schema (so several
    // publishers may share one topic) and REFUSED for a conflicting one —
    // spec §7 clause 3, tightened from "may be rejected" to "must be rejected".
    // This used to overwrite the cached schema silently, which meant a
    // subscriber's next row was decoded against a shape nobody agreed to.
    if (slot.declared.has_value()) {
        if (incoming.ConflictsWith(*slot.declared)) {
            throw std::runtime_error(
                "InProcessPubSubProvider: topic already declared with a conflicting schema: " +
                key);
        }
        return;  // identical (or non-comparable) re-declaration — no-op
    }

    slot.declared = std::move(incoming);
    if (schema) {
        slot.schema = MakeSharedSchema(OwnedSchema::DeepCopy(schema.get()));
    }
}

// mu_ is held across the callback: one delivery at a time, so a callback must not re-enter.
void InProcessPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                      const RowEncoder& encoder, const Attachments& attachments) {
    VectorWriteBuffer wb;
    encoder(wb);
    const std::vector<uint8_t> buf = wb.Finish();

    std::lock_guard lock(impl_->mu);
    auto [it, _] = impl_->topics.try_emplace(internal::JoinSegments(topic_segments));
    // Copy-to-locals before dispatch (HARD-4's pattern): a callback that re-enters
    // Unsubscribe() would otherwise null the std::function being invoked. Dispatch stays
    // under mu_ so the delivery contract's one-callback-at-a-time clause holds; mu_ is
    // non-recursive, so a re-entering callback deadlocks rather than corrupting — which is
    // what the contract forbids.
    const SubscribeCallback cb = it->second.callback;
    const SharedSchema schema = it->second.schema;
    if (cb) {
        cb(buf.data(), buf.size(), schema, attachments);
    }
}

SubscriptionResult InProcessPubSubProvider::Subscribe(
    const std::vector<std::string>& topic_segments, SubscribeCallback callback) {
    std::lock_guard lock(impl_->mu);
    auto& slot = impl_->topics[internal::JoinSegments(topic_segments)];
    slot.callback = std::move(callback);
    return {MakeReadySchemaFuture(slot.schema)};
}

void InProcessPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    std::lock_guard lock(impl_->mu);
    auto it = impl_->topics.find(internal::JoinSegments(topic_segments));
    if (it != impl_->topics.end()) {
        it->second.callback = nullptr;
    }
}

}  // namespace fletcher
