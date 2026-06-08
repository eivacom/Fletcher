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
/// obtain it: the future resolves with a non-null SharedSchema once the
/// schema is known — immediately for providers that already hold it, or
/// asynchronously when a publisher announces it (late-joining subscribers).
/// Consumers may ignore the future, or wait on it (`get()`/`wait_for()`) if
/// they need the schema out-of-band. Either way the per-sample SharedSchema
/// delivered to the SubscribeCallback carries the schema for each row.
struct SubscriptionResult {
    std::shared_future<SharedSchema> schema;
};

/// Build a SubscriptionResult schema future that is already resolved with
/// `schema`. For providers that know the schema synchronously at Subscribe
/// time (in-process loopback, mocks, and — for now — publisher-first DDS).
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
    /// one topic); providers may reject a re-declaration with a conflicting
    /// schema.
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             OwnedSchema schema) = 0;

    /// Callback that encodes a row directly into a WriteBuffer.
    using RowEncoder = std::function<void(WriteBuffer&)>;

    /// Publish by writing the encoded row directly into the provider's
    /// buffer.  The provider supplies a WriteBuffer; the encoder writes
    /// into it.
    virtual void Publish(const std::vector<std::string>& topic_segments, RowEncoder encoder,
                         const Attachments& attachments = {}) = 0;

    /// Callback signature for Subscribe — delivers raw encoded row bytes,
    /// the topic's schema, and any sidecar attachments.
    /// The SharedSchema keeps the schema alive as long as any copy exists,
    /// so callbacks may safely store it for later use.
    using SubscribeCallback = std::function<void(const uint8_t* data, size_t len,
                                                 SharedSchema schema, Attachments attachments)>;

    /// Subscribe to a named topic.  Returns the schema that the
    /// publisher provided when it created the topic.
    virtual SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                         SubscribeCallback callback) = 0;

    /// Remove a previously registered subscription.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_
