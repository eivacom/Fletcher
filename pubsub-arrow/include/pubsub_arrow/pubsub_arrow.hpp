#ifndef FLETCHER_INCLUDE_PUBSUB_ARROW_HPP_
#define FLETCHER_INCLUDE_PUBSUB_ARROW_HPP_

#include <pubsub/driver.hpp>
#include <pubsub/pubsub.hpp>
#include <arrow_bridge/codec.hpp>

#include <arrow/type_fwd.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fletcher {

/// Arrow-friendly wrapper around the nanoarrow-based PubSub Driver.
///
/// PubSubArrow bridges the gap between the nanoarrow pub/sub core (which
/// deals with raw bytes and ArrowSchema C structs) and server-side code
/// that works with Arrow C++ types (arrow::Schema, ArrowRow).
///
/// Edge-deployed code uses the Driver directly; server-side code uses
/// PubSubArrow for convenience.
class PubSubArrow {
 public:
    explicit PubSubArrow(std::shared_ptr<PubSub> provider);

    /// Create a topic with an Arrow C++ schema.
    /// Passing nullptr is allowed (topic created without schema).
    void CreateTopic(const std::vector<std::string>& segments,
                     std::shared_ptr<arrow::Schema> schema,
                     std::any config = {});

    /// Publish an ArrowRow (encoded via Codec).
    void Publish(const std::vector<std::string>& segments,
                 const ArrowRow& row,
                 const Attachments& attachments = {});

    /// Publish using a direct encoder (passthrough to Driver).
    void PublishDirect(const std::vector<std::string>& segments,
                       PubSub::RowEncoder encoder,
                       const Attachments& attachments = {});

    struct SubscribeResult {
        uint64_t subscription_id;
        std::shared_ptr<arrow::Schema> schema;
    };

    /// Subscribe with ArrowRow delivery.
    using SubscribeCallback = std::function<void(ArrowRow row, Attachments attachments)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments,
                              SubscribeCallback callback,
                              std::any config = {});

    void Unsubscribe(uint64_t subscription_id);

    std::vector<std::string> ListTopics() const;
    bool HasTopic(const std::vector<std::string>& segments) const;

 private:
    std::unique_ptr<Driver> driver_;

    mutable std::mutex mu_;
    struct TopicCodec {
        std::shared_ptr<arrow::Schema> arrow_schema;
        std::unique_ptr<Codec> codec;
    };
    std::unordered_map<std::string, TopicCodec> codecs_;

    static std::string JoinSegments(const std::vector<std::string>& segs);
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_ARROW_HPP_
