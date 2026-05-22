// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_PUBSUB_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PUBSUB_HPP_

#include <any>
#include <cstdint>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/owned_schema.hpp"

namespace fletcher {

/// Result returned by PubSub::Subscribe, carrying the schema
/// that the publisher registered when it created the topic.
struct SubscriptionResult {
    OwnedSchema schema;
};

/// Abstract transport provider for pub/sub.
///
/// The provider deals with raw byte buffers and nanoarrow schemas —
/// no Apache Arrow C++ dependency.  Higher-level wrappers (PubSubArrow)
/// add ArrowRow convenience for server-side code that needs it.
///
/// Topic names are represented as a list of string segments so that
/// the provider can join them with any separator it prefers.
class PubSub {
   public:
    virtual ~PubSub() = default;

    /// Called once per topic by the publisher.  The schema describes the
    /// Arrow structure of rows on this topic.  Ownership of the schema
    /// is transferred to the provider.
    ///
    /// @param config  Provider-specific topic configuration (e.g. QoS).
    ///                The provider interprets this via std::any_cast to
    ///                its own config struct.  Empty = provider defaults.
    virtual void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema,
                             std::any config = {}) = 0;

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
    ///
    /// @param config  Provider-specific subscriber configuration (e.g. QoS).
    ///                Empty = provider defaults.
    virtual SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                         SubscribeCallback callback, std::any config = {}) = 0;

    /// Remove a previously registered subscription.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PUBSUB_HPP_
