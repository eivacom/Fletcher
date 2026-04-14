#ifndef FLETCHER_INCLUDE_PUBSUB_PUBSUB_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PUBSUB_HPP_

#include "pubsub/owned_schema.hpp"
#include "pubsub/types.hpp"

#include "pubsub/write_buffer.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             OwnedSchema schema) = 0;

    /// Callback that encodes a row directly into a WriteBuffer.
    using RowEncoder = std::function<void(WriteBuffer&)>;

    /// Publish by writing the encoded row directly into the provider's
    /// buffer.  The provider supplies a WriteBuffer; the encoder writes
    /// into it.
    virtual void Publish(const std::vector<std::string>& topic_segments,
                         RowEncoder encoder,
                         const Attachments& attachments = {}) = 0;

    /// Callback signature for Subscribe — delivers raw encoded row bytes
    /// and any sidecar attachments.
    using SubscribeCallback = std::function<void(const uint8_t* data,
                                                 size_t len,
                                                 Attachments attachments)>;

    /// Subscribe to a named topic.  Returns the schema that the
    /// publisher provided when it created the topic.
    virtual SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments,
        SubscribeCallback callback) = 0;

    /// Remove a previously registered subscription.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PUBSUB_HPP_
