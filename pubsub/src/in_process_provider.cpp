// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/in_process_provider.hpp"

#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fletcher/pubsub/internal/schema_conflict.hpp"
#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

struct InProcessPubSubProvider::Impl {
    struct TopicState {
        SubscribeCallback callback;
        // What the LIVE subscription was told, latched when Subscribe returned.
        // Not the same thing as `schema` below: in kAsDeclared a declaration
        // that lands after a subscription exists must never reach it, so the
        // subscription keeps delivering exactly what its SchemaArrival reported
        // until the client resubscribes.
        SharedSchema subscription_schema;
        // The write end of that subscription's arrival, held only in kCarried
        // while no schema has been declared yet. Destroying it unresolved is
        // kSubscriptionEnded, which is what a teardown before a declaration is.
        std::optional<SchemaResolver> resolver;
        // Null when nobody announced one; in kAsDeclared the gateway lets the
        // client bring its own.
        SharedSchema schema;
        // Set once a publisher has declared this topic. Absent for a topic a
        // subscriber or a publish created lazily, which is why it is optional
        // rather than a flag beside empty bytes: "declared with no schema" and
        // "never declared" are different states, and only the first can conflict.
        std::optional<internal::DeclaredSchema> declared;
    };

    explicit Impl(SchemaCarriage carriage_in) : carriage(carriage_in) {}

    SchemaCarriage carriage;
    std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
};

InProcessPubSubProvider::InProcessPubSubProvider(SchemaCarriage carriage)
    : impl_(std::make_unique<Impl>(carriage)) {}

InProcessPubSubProvider::~InProcessPubSubProvider() = default;

void InProcessPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                          OwnedSchema schema) {
    // Every seam entry point translates, so the only exception leaving this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        std::string key = internal::JoinSegments(topic_segments);

        if (impl_->carriage == SchemaCarriage::kCarried && !schema) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "InProcessPubSubProvider: a schema-carrying instance cannot declare "
                              "a topic with no schema: " +
                                  key);
        }

        // Encode before taking the lock, so the locked section is a byte compare
        // rather than an IPC encode every concurrent CreateTopic queues behind.
        // The SAME comparison Publisher::CreateTopic uses one layer up — deliberately
        // not a second implementation of it.
        internal::DeclaredSchema incoming = internal::DeclaredSchema::Encode(schema.get());

        SharedSchema shared;
        if (schema) {
            shared = MakeSharedSchema(OwnedSchema::DeepCopy(schema.get()));
        }

        std::optional<SchemaResolver> to_resolve;
        {
            std::lock_guard lock(impl_->mu);
            auto& slot = impl_->topics[key];

            // Re-declaration is idempotent for an identical schema (so several
            // publishers may share one topic) and REFUSED for a conflicting one —
            // spec §7 clause 3, tightened from "may be rejected" to "must be rejected".
            // This used to overwrite the cached schema silently, which meant a
            // subscriber's next row was decoded against a shape nobody agreed to.
            if (slot.declared.has_value()) {
                if (incoming.ConflictsWith(*slot.declared)) {
                    throw PubSubError(PubSubStatus::kSchemaConflict,
                                      "InProcessPubSubProvider: topic already declared with a "
                                      "conflicting schema: " +
                                          key);
                }
                return;  // identical (or non-comparable) re-declaration — no-op
            }

            slot.declared = std::move(incoming);
            slot.schema = shared;

            // In kCarried a subscription that already exists is still waiting for
            // its schema, so this declaration IS its arrival — and every delivery
            // it ever sees carries this schema, so nothing is mixed.
            //
            // In kAsDeclared it is deliberately NOT: `subscription_schema` is left
            // exactly as Subscribe latched it, so a live subscription cannot flip
            // from null to non-null mid-stream (§7 clause 1). A client that wants
            // the newly declared shape resubscribes.
            if (impl_->carriage == SchemaCarriage::kCarried) {
                slot.subscription_schema = shared;
                if (slot.resolver.has_value()) {
                    to_resolve = std::move(slot.resolver);
                    slot.resolver.reset();
                }
            }
        }

        // Outside the lock: resolving wakes waiters, and none of them should be
        // woken into a thread that still holds the provider mutex.
        if (to_resolve.has_value()) {
            std::move(*to_resolve).Resolve(shared);
        }
    });
}

// mu_ is held across the callback: one delivery at a time, so a callback must not re-enter.
void InProcessPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                      const RowEncoder& encoder, const Attachments& attachments) {
    TranslateSeamFailure([&] {
        VectorWriteBuffer wb;
        encoder(wb);
        const std::vector<uint8_t> buf = wb.Finish();

        std::string key = internal::JoinSegments(topic_segments);

        std::lock_guard lock(impl_->mu);
        auto [it, _] = impl_->topics.try_emplace(std::move(key));

        // Schema-before-data on a carrying instance, held by refusal: there is no
        // implicit declaration, and no sample can be delivered with a schema that
        // does not exist yet.
        if (impl_->carriage == SchemaCarriage::kCarried && !it->second.declared.has_value()) {
            throw PubSubError(
                PubSubStatus::kTopicNotDeclared,
                "InProcessPubSubProvider: publish to an undeclared topic: " + it->first);
        }

        // Copy-to-locals before dispatch (HARD-4's pattern): a callback that re-enters
        // Unsubscribe() would otherwise null the std::function being invoked. Dispatch stays
        // under mu_ so the delivery contract's one-callback-at-a-time clause holds; mu_ is
        // non-recursive, so a re-entering callback deadlocks rather than corrupting — which is
        // what the contract forbids.
        const SubscribeCallback cb = it->second.callback;
        // What this SUBSCRIPTION was told, not what the topic currently holds.
        const SharedSchema schema = it->second.subscription_schema;
        if (cb) {
            cb(buf.data(), buf.size(), schema, attachments);
        }
    });
}

SubscriptionResult InProcessPubSubProvider::Subscribe(
    const std::vector<std::string>& topic_segments, SubscribeCallback callback) {
    return TranslateSeamFailure([&]() -> SubscriptionResult {
        std::string key = internal::JoinSegments(topic_segments);

        std::lock_guard lock(impl_->mu);
        auto& slot = impl_->topics[key];
        slot.callback = std::move(callback);
        // Dropping any previous subscription's resolver reports kSubscriptionEnded
        // to whoever still holds that arrival — one callback per topic per
        // instance (§7 clause 4), so the old subscription really is over.
        slot.resolver.reset();

        if (impl_->carriage == SchemaCarriage::kCarried) {
            if (slot.schema) {
                slot.subscription_schema = slot.schema;
                return SubscriptionResult{SchemaArrival::Ready(slot.schema)};
            }
            // Nothing declared yet: the arrival stays open until a publisher
            // declares the topic, or until this subscription ends.
            auto [arrival, resolver] = SchemaArrival::Create();
            slot.resolver.emplace(std::move(resolver));
            return SubscriptionResult{std::move(arrival)};
        }

        // kAsDeclared: latched HERE, once, for this subscription's whole life —
        // whatever is declared right now, which may legitimately be null.
        slot.subscription_schema = slot.schema;
        return SubscriptionResult{SchemaArrival::Ready(slot.subscription_schema)};
    });
}

void InProcessPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    TranslateSeamFailure([&] {
        std::optional<SchemaResolver> ended;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->topics.find(internal::JoinSegments(topic_segments));
            if (it == impl_->topics.end()) return;
            it->second.callback = nullptr;
            it->second.subscription_schema = nullptr;
            ended = std::move(it->second.resolver);
            it->second.resolver.reset();
        }
        // Destroyed outside the lock: unresolved means kSubscriptionEnded, which
        // is precisely what "torn down before the schema arrived" is.
    });
}

}  // namespace fletcher
