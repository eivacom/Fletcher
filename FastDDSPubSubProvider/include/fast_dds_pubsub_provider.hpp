#ifndef FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <pubsub/pubsub_provider.hpp>

#include <cstdint>
#include <memory>

namespace fletcher {

/// PubSubProvider backed by eProsima Fast DDS.
///
/// DataWriter / DataReader QoS is set to RELIABLE reliability,
/// KEEP_ALL history, and TRANSIENT_LOCAL durability so that messages
/// are not silently dropped and late-joining subscribers receive
/// historical samples.
class FastDDSPubSubProvider : public PubSubProvider {
 public:
    /// @param domain_id         DDS domain ID (default 0).
    /// @param max_payload_bytes Maximum serialized envelope in bytes (default 1 MB).
    explicit FastDDSPubSubProvider(uint32_t domain_id = 0,
                                   uint32_t max_payload_bytes = 1024 * 1024);
    ~FastDDSPubSubProvider() override;

    FastDDSPubSubProvider(const FastDDSPubSubProvider&) = delete;
    FastDDSPubSubProvider& operator=(const FastDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments,
                     std::shared_ptr<arrow::Schema> schema) override;

    void Publish(const std::vector<std::string>& topic_segments,
                 const Envelope& envelope) override;

    SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments,
        SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FAST_DDS_PUBSUB_PROVIDER_HPP_
