#ifndef FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_

#include "pubsub/types.hpp"

#include <row_codec.hpp>
#include <write_buffer.hpp>

#include <arrow/type_fwd.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
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
/// subscription delivery.  The provider encodes/decodes ArrowRow data
/// internally — callers work with typed Arrow scalars, never raw bytes.
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
    /// companion topic with durable semantics).  It also creates a
    /// RowCodec for internal encoding/decoding.
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             std::shared_ptr<arrow::Schema> schema) = 0;

    /// Publish an ArrowRow with optional attachments to a named topic.
    /// The provider encodes the row internally.
    virtual void Publish(const std::vector<std::string>& topic_segments,
                         const ArrowRow& row,
                         const Attachments& attachments = {}) = 0;

    /// Callback that encodes a row directly into a WriteBuffer.
    using RowEncoder = std::function<void(WriteBuffer&)>;

    /// Publish by writing the encoded row directly into the provider's
    /// buffer.  The provider supplies a WriteBuffer; the encoder writes
    /// into it.  Override in transport providers for zero-copy.
    ///
    /// The default implementation encodes into a temp vector, decodes
    /// back to ArrowRow via the schema passed to CreateTopic, and
    /// calls Publish().
    virtual void PublishDirect(const std::vector<std::string>& topic_segments,
                               RowEncoder encoder,
                               const Attachments& attachments = {});

    /// Callback signature for Subscribe — delivers decoded ArrowRow
    /// and any sidecar attachments.
    using SubscribeCallback = std::function<void(ArrowRow row,
                                                 Attachments attachments)>;

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

    /// Register a RowCodec for a topic so the default PublishDirect
    /// fallback can decode pre-encoded rows.  Call after CreateTopic.
    void RegisterCodec(const std::string& topic_key,
                       std::shared_ptr<arrow::Schema> schema);

 private:
    RowCodec* FindCodec(const std::string& topic_key) const;
    std::unordered_map<std::string, std::unique_ptr<RowCodec>> codecs_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PUBSUB_PROVIDER_HPP_
