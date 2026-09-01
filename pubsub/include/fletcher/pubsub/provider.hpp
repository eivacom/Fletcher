// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_

#include <cstdint>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/owned_schema.hpp"

namespace fletcher {

/// Result returned by PubSubProvider::Subscribe.
///
/// `schema` is a future for the topic's schema. Subscribe never blocks to
/// obtain it: the future resolves once the schema is known — immediately for
/// providers that already hold it, or asynchronously when a publisher announces
/// it (late-joining subscribers).
///
/// What it resolves WITH follows the transport's schema mode, and the two are
/// never mixed (§7 clause 1 of docs/pubsub-interface-spec.md):
///  - a schema-**carrying** transport resolves it with a non-null SharedSchema;
///  - a transport that carries no schemas at all — the in-process loopback,
///    where the client brings its own — resolves it with **null**.
///
/// Either way it does resolve, so a consumer that waits never hangs. Consumers
/// may ignore the future, or wait on it (`get()`/`wait_for()`) if they need the
/// schema out-of-band; the per-sample SharedSchema delivered to the
/// SubscribeCallback carries the schema for each row.
struct SubscriptionResult {
    std::shared_future<SharedSchema> schema;
};

/// Build a SubscriptionResult schema future that is already resolved with
/// `schema`. For providers that know the schema synchronously at Subscribe
/// time (in-process loopback, mocks, and — for now — publisher-first DDS).
///
/// `schema` may legitimately be null: that is how a schema-less transport
/// resolves the future (see SubscriptionResult), and it is why this returns a
/// resolved-with-null future rather than refusing.
inline std::shared_future<SharedSchema> MakeReadySchemaFuture(SharedSchema schema) {
    std::promise<SharedSchema> p;
    p.set_value(std::move(schema));
    return p.get_future().share();
}

/// Abstract transport provider for pub/sub.
///
/// The provider deals with raw byte buffers and nanoarrow schemas —
/// no Apache Arrow C++ dependency.  Higher-level wrappers (Publisher,
/// Subscriber, PublisherArrow, SubscriberArrow) add convenience APIs
/// on top of this interface.
///
/// Topic names are represented as a list of string segments so that
/// the provider can join them with any separator it prefers.
///
/// Provider semantics: ONE callback per topic. Fan-out (multiple
/// local subscribers to the same topic) is handled by the Subscriber
/// class, not by providers.
///
/// Provider-specific configuration (e.g. QoS) is supplied at provider
/// construction time via a provider-specific Options struct. There is
/// no per-call config parameter.
class PubSubProvider {
   public:
    virtual ~PubSubProvider() = default;

    /// Declares a topic and its schema; called on the publisher side. The
    /// schema describes the Arrow structure of rows on this topic and its
    /// ownership is transferred to the provider. Subscribers do not call this —
    /// they learn the schema out-of-band (see Subscribe). Re-declaring a topic
    /// with an identical schema is idempotent (so several publishers may share
    /// one topic); a provider **must** reject a re-declaration with a
    /// conflicting schema, by throwing (spec §7 clause 3 — "may" became "must"
    /// with the 2026-09-01 ruling, so a provider that silently overwrote the
    /// declared schema is now non-conforming).
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             OwnedSchema schema) = 0;

    /// Callback that encodes a row directly into a WriteBuffer.
    using RowEncoder = std::function<void(WriteBuffer&)>;

    /// Publish by writing the encoded row directly into the provider's
    /// buffer.  The provider supplies a WriteBuffer; the encoder writes
    /// into it.
    virtual void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                         const Attachments& attachments = {}) = 0;

    /// Callback signature for Subscribe — delivers raw encoded row bytes,
    /// the topic's schema, and any sidecar attachments.
    /// `schema` and `attachments` are **borrowed for the duration of the call**, like `data`. A
    /// callback that wants to keep either one copies it — SharedSchema is a shared_ptr, so a copy
    /// keeps the schema alive for as long as that copy lives.
    ///
    /// They were passed by value until it was measured. An empty `Attachments` is an
    /// `unordered_map`, and MSVC allocates a sentinel node in its default constructor, so every
    /// delivery on every topic built and destroyed one whether or not the sample carried any:
    /// **~110 ns per sample against 1.4 ns for the call itself**, more than the whole rest of the
    /// delivery path. See "Measured decisions" in the FastDDS provider README.
    ///
    /// Delivery contract every provider must uphold:
    ///  - **Schema before data.** The callback is never invoked with a null
    ///    schema. A subscriber may Subscribe before any publisher exists; the
    ///    schema then arrives asynchronously, and the provider buffers data
    ///    that arrives ahead of it and delivers that data only once the schema
    ///    is known. A transport that carries no schemas at all - the gateway's in-process
    ///    loopback, where the client brings its own - passes null throughout instead, and
    ///    must never mix the two.
    ///  - **Per-writer order.** Samples from a single writer reach the callback
    ///    in the order they were published. This holds across the schema
    ///    handoff too: the buffered pre-schema backlog is delivered before —
    ///    and never interleaved with — samples that arrive live afterwards.
    ///  - **One callback at a time.** Never two deliveries in flight for the same
    ///    subscription, though the thread they arrive on may differ between samples. A
    ///    provider that fans out from several threads must serialise them itself.
    using SubscribeCallback =
        std::function<void(const uint8_t* data, size_t len, const SharedSchema& schema,
                           const Attachments& attachments)>;

    /// Subscribe to a named topic. **Never blocks**: a subscriber may subscribe
    /// before any publisher exists, and the returned SubscriptionResult carries
    /// a *future* for the schema rather than the schema itself — see
    /// SubscriptionResult for what it resolves with, and when. Delivery obeys
    /// the SubscribeCallback contract above (schema-before-data, per-writer
    /// order).
    ///
    /// `data` may point into a buffer the transport owns rather than a copy of one — that is a
    /// provider-local optimisation, not part of this contract. Either way the pointer is only
    /// valid for the duration of the call and was never owned by the callback.
    [[nodiscard]] virtual SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments, SubscribeCallback callback) = 0;

    /// Remove a previously registered subscription. **Once this returns, no
    /// further callback runs for that topic** (§7 clause 6): a provider that
    /// delivers from its own thread must not let a delivery already in flight
    /// outlive the call. Unsubscribing a topic with no subscription is a no-op,
    /// not an error, so it is safe to call unconditionally on teardown.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_
