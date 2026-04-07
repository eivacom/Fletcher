#ifndef FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_

#include "pubsub/envelope.hpp"

#include <arrow/type_fwd.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fletcher {

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

    /// Called once per topic before any Publish / Subscribe calls.
    /// The schema describes the Arrow structure of the rows that will
    /// flow on this topic.  The provider may use it for type metadata,
    /// logging, or schema registry integration.
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             std::shared_ptr<arrow::Schema> schema) = 0;

    /// Publish an envelope (encoded row + optional attachments) to a
    /// named topic.
    virtual void Publish(const std::vector<std::string>& topic_segments,
                         const Envelope& envelope) = 0;

    /// Callback signature for Subscribe.
    using SubscribeCallback = std::function<void(const Envelope&)>;

    /// Register a subscription callback for a named topic.
    /// Each call replaces a previously registered callback (if any).
    virtual void Subscribe(const std::vector<std::string>& topic_segments,
                           SubscribeCallback callback) = 0;

    /// Remove a previously registered subscription.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_
