// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_SUBSCRIBER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_SUBSCRIBER_HPP_

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/provider.hpp"

namespace fletcher {

/// High-level subscription manager. Supports multiple local subscribers
/// per topic via internal fan-out: the first Subscribe() call for a
/// given topic creates a single provider-level subscription with a
/// multiplex callback; subsequent Subscribe() calls on the same topic
/// just register additional callbacks. The last Unsubscribe() for a
/// topic releases the provider-level subscription.
///
/// Construct with a shared_ptr to any PubSubProvider implementation.
/// Multiple Subscriber instances against the same provider are legal
/// but will each create their own provider-level subscription — the
/// fan-out only deduplicates within one Subscriber.
///
/// Thread safety: all public methods are safe to call from any thread.
class Subscriber {
   public:
    explicit Subscriber(std::shared_ptr<PubSubProvider> provider);
    ~Subscriber();

    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    /// Result returned by Subscribe. `schema` is a future for the topic's
    /// schema (see SubscriptionResult): non-blocking, resolves with a
    /// non-null SharedSchema once known. Shared across fan-out subscribers
    /// to the same topic.
    struct SubscribeResult {
        uint64_t subscription_id;
        std::shared_future<SharedSchema> schema;
    };

    /// User callback. The first parameter is the subscription_id this
    /// callback was registered under, so callers (e.g. the gateway WS
    /// session) can correlate samples with the subscription without
    /// racing against the Subscribe() return.
    using SubscribeCallback =
        std::function<void(uint64_t subscription_id, const uint8_t* data, size_t len,
                           SharedSchema schema, Attachments attachments)>;

    /// Subscribe to a topic. Returns a per-subscription ID for targeted
    /// unsubscribe and the schema that the publisher registered.
    SubscribeResult Subscribe(const std::vector<std::string>& segments, SubscribeCallback cb);

    /// Remove a subscription by ID. Calls provider->Unsubscribe if this
    /// was the last subscription on the topic.
    ///
    /// Unsubscribing does NOT guarantee that no further callbacks fire.
    /// A subscriber may still receive one final in-flight message after
    /// Unsubscribe returns. The fan-out path snapshots the topic's
    /// (id, callback) pairs under the lock, releases the lock, then
    /// invokes the callbacks (see subscriber.cpp). If Unsubscribe runs
    /// in the window between that snapshot and the invocation, this
    /// subscription's callback is already captured in the snapshot and
    /// will be called one last time. This is intentional and by design
    /// (copy-then-release-then-call fan-out), not a bug.
    void Unsubscribe(uint64_t subscription_id);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_SUBSCRIBER_HPP_
