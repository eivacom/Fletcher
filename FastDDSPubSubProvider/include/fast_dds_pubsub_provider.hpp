#ifndef FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <pubsub/pubsub.hpp>

#include <cstdint>
#include <memory>

namespace fletcher {

/// PubSub transport backed by eProsima Fast DDS.
///
/// DataWriter / DataReader QoS is set to RELIABLE reliability,
/// KEEP_ALL history, and TRANSIENT_LOCAL durability so that messages
/// are not silently dropped and late-joining subscribers receive
/// historical samples.
class FastDDSPubSubProvider : public PubSub {
 public:
    /// @param domain_id         DDS domain ID (default 0).
    /// @param max_payload_bytes Maximum serialized envelope in bytes (default 1 MB).
    explicit FastDDSPubSubProvider(uint32_t domain_id = 0,
                                   uint32_t max_payload_bytes = 1024 * 1024);
    ~FastDDSPubSubProvider() override;

    FastDDSPubSubProvider(const FastDDSPubSubProvider&) = delete;
    FastDDSPubSubProvider& operator=(const FastDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments,
                     OwnedSchema schema,
                     std::any config = {}) override;

    void Publish(const std::vector<std::string>& topic_segments,
                 RowEncoder encoder,
                 const Attachments& attachments = {}) override;

    SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments,
        SubscribeCallback callback,
        std::any config = {}) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
