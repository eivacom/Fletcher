#ifndef FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_

#include "pubsub/envelope.hpp"

#include <arrow/type_fwd.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fletcher {

/// Result returned by PubSubProvider::Subscribe, carrying the schema
/// that the publisher registered when it created the topic.
struct SubscriptionResult {
    std::shared_ptr<arrow::Schema> schema;
};

/// Abstract transport provider for Arrow-row pub/sub.
///
/// Implementations supply topic creation, message publication, and
/// subscription delivery.  The binary payload exchanged over the wire
/// is an `Envelope` containing an `EncodedRow` and optional sidecar
/// `Attachments` (key/value blob pairs).
///
/// Topic names are represented as a list of string segments so that
/// the provider can join them with any separator it prefers (dots,
/// slashes, etc.).
class PubSubProvider {
 public:
    virtual ~PubSubProvider() = default;

    /// Called once per topic by the publisher.  The schema describes the
    /// Arrow structure of rows on this topic.  The provider stores the
    /// schema and makes it available to future subscribers (e.g. via a
    /// companion topic with durable semantics).
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             std::shared_ptr<arrow::Schema> schema) = 0;

    /// Publish an envelope (encoded row + optional attachments) to a
    /// named topic.
    virtual void Publish(const std::vector<std::string>& topic_segments,
                         const Envelope& envelope) = 0;

    /// Callback signature for Subscribe.
    using SubscribeCallback = std::function<void(const Envelope&)>;

    /// Subscribe to a named topic.  Returns the schema that the
    /// publisher provided when it created the topic.
    ///
    /// The provider retrieves the schema from the transport layer
    /// (e.g. a companion topic) before returning.  Callers do not
    /// need to call CreateTopic before subscribing.
    virtual SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments,
        SubscribeCallback callback) = 0;

    /// Remove a previously registered subscription.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_
