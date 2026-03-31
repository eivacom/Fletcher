#ifndef ARROW_ROW_INCLUDE_PUBSUB_PROVIDER_HPP_
#define ARROW_ROW_INCLUDE_PUBSUB_PROVIDER_HPP_

#include <arrow/api.h>
#include <row_codec.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace arrow_row {

// Abstract transport layer for pub/sub over Arrow-encoded rows.
//
// Implement this for your concrete protocol (DDS, MQTT, Zenoh, etc.).
// The topic name is delivered as a vector of segments — the implementation
// is free to join them with dots, slashes, or any separator that suits
// the underlying transport.
//
// The binary payload exchanged over the wire is always a single EncodedRow
// (i.e. RowCodec::EncodeRow output).
class PubSubProvider {
 public:
    virtual ~PubSubProvider() = default;

    // Called once per topic before any Publish/Subscribe.
    // The schema describes the row layout so the provider can propagate
    // type information to the transport if needed.
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             std::shared_ptr<arrow::Schema> schema) = 0;

    // Publish a single encoded row to the named topic.
    virtual void Publish(const std::vector<std::string>& topic_segments,
                         const EncodedRow& row) = 0;

    // Subscribe to incoming rows on the named topic.
    // The callback receives raw EncodedRow bytes — decoding is handled
    // by the generated topic class, not the provider.
    using SubscribeCallback = std::function<void(const EncodedRow&)>;
    virtual void Subscribe(const std::vector<std::string>& topic_segments,
                           SubscribeCallback callback) = 0;

    // Remove a previously registered subscription.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_PUBSUB_PROVIDER_HPP_
