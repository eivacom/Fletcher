// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_SUBSCRIBER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_SUBSCRIBER_HPP_

#include <cstdint>
#include <functional>
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

    /// Retires and drains every remaining subscription before releasing the
    /// provider-level ones, so §7 clause 6 holds through teardown as well: no
    /// callback of this Subscriber is running when the destructor returns, and
    /// none begins afterwards. Same wait, same one carve-out as Unsubscribe.
    ~Subscriber();

    Subscriber(const Subscriber&) = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    /// Result returned by Subscribe. `schema` is a waitable arrival for the
    /// topic's schema (see SubscriptionResult and SchemaArrival): non-blocking,
    /// answered once the schema is known. Shared across fan-out subscribers to
    /// the same topic — they observe one provider subscription, so they observe
    /// one arrival.
    struct SubscribeResult {
        uint64_t subscription_id;
        SchemaArrival schema;
    };

    /// User callback. The first parameter is the subscription_id this
    /// callback was registered under, so callers (e.g. the gateway WS
    /// session) can correlate samples with the subscription without
    /// racing against the Subscribe() return.
    using SubscribeCallback =
        std::function<void(uint64_t subscription_id, const uint8_t* data, size_t len,
                           const SharedSchema& schema, const Attachments& attachments)>;

    /// Subscribe to a topic. Returns a per-subscription ID for targeted
    /// unsubscribe and the schema that the publisher registered.
    [[nodiscard]] SubscribeResult Subscribe(const std::vector<std::string>& segments,
                                            SubscribeCallback cb);

    /// Remove a subscription by ID. Calls provider->Unsubscribe if this
    /// was the last subscription on the topic.
    ///
    /// **Once this returns, that callback is not running and will not run
    /// again** (spec §7 clause 6, which binds at every tier this seam
    /// publishes). Concretely: no invocation of that subscription's callback
    /// begins after this returns, and none is in progress when it does — so on
    /// return the caller may free or unpin whatever the callback was using.
    /// **Unsubscribe therefore BLOCKS** while a delivery for that subscription
    /// is in flight, for as long as that callback takes; the seam cannot bound
    /// foreign callback duration, so a callback that never returns blocks it
    /// forever.
    ///
    /// **The one shape where the caller may NOT free callback state on return:**
    /// an Unsubscribe *issued from inside a delivery callback on this
    /// Subscriber* does not wait — a cancellation cannot wait for the frame it
    /// is already in, and waiting for a *sibling* frame on the same Subscriber
    /// is exactly what makes two handlers hang one another. Such a call still
    /// guarantees the first half (no invocation begins afterwards); it does not
    /// guarantee the second (nothing is in progress). Two handlers on
    /// **different** Subscriber objects that cancel each other can still block
    /// one another — see integration-tests/pubsub-conformance/README.md.
    ///
    /// **Cancelling something that is not live is a no-op, not an error.** An
    /// unknown id, an already-cancelled id and an id this Subscriber never
    /// issued are all accepted and do nothing, so teardown may call this
    /// unconditionally — a foreign-runtime finaliser cannot let an exception
    /// escape. The cost is deliberate: a mistyped id is ignored rather than
    /// reported.
    ///
    /// **A subscription id is meaningful only to the Subscriber that issued
    /// it.** Ids are per-instance counters, so handing one to a *different*
    /// Subscriber silently addresses that instance's own subscription with the
    /// same number, or does nothing. This predates the guarantees above and is
    /// unchanged by them.
    void Unsubscribe(uint64_t subscription_id);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_SUBSCRIBER_HPP_
