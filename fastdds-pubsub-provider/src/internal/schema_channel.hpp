// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The companion __schema channel's per-subscription handoff: a promise, resolved when the topic's
// schema is known.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_

#include <chrono>
#include <fletcher/core/status.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/schema_arrival.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace fletcher {
namespace internal {

// Per-subscription schema handoff. Resolved either on the spot, by an application thread, when
// this provider already holds the topic's schema (EnsureSchemaChannel,
// fast_dds_pubsub_provider.cpp), or by this provider's one schema thread (HandleSchema, same file)
// when the companion __schema sample arrives.
//
// Guarded by its OWN mutex, `m` -- never `impl_->mu` or `schema_mu`. `m` is always the innermost
// lock on every path that reaches it (an application thread holds `impl_->mu`, or nothing at all;
// the schema thread holds `schema_mu`), so it adds no cycle to the documented impl_->mu ->
// schema_mu order: nothing here ever calls back into either.
//
// The resolver is single-use by construction, so "resolved twice" is unrepresentable rather than
// guarded by a `resolved` flag.
struct SchemaChannel {
    SchemaChannel() {
        auto pair = SchemaArrival::Create();
        arrival = std::move(pair.first);
        resolver.emplace(std::move(pair.second));
    }

    std::mutex m;
    SchemaArrival arrival;
    std::optional<SchemaResolver> resolver;

    void Resolve(SharedSchema schema) {
        std::lock_guard<std::mutex> lk(m);
        if (!resolver.has_value()) return;
        std::optional<SchemaResolver> token = std::move(resolver);
        resolver.reset();
        std::move(*token).Resolve(std::move(schema));
    }

    // Unsubscribed before the schema arrived. Dropping the token unresolved IS the outcome --
    // kSubscriptionEnded -- which is what the broken promise used to say by throwing out of get(),
    // only now it is a value a binding can read and cannot confuse with "this transport carries no
    // schemas".
    void Break() {
        std::lock_guard<std::mutex> lk(m);
        resolver.reset();
    }

    // The transport could not open the channel at all. Distinct from Break: a waiter must not read
    // a fault as the normal end of a subscription.
    void Fail(PubSubStatus status, std::string message) {
        std::lock_guard<std::mutex> lk(m);
        if (!resolver.has_value()) return;
        std::optional<SchemaResolver> token = std::move(resolver);
        resolver.reset();
        std::move(*token).Fail(status, std::move(message));
    }
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_
