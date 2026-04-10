#ifndef XRCE_DDS_PUBSUB_PROVIDER_HPP_
#define XRCE_DDS_PUBSUB_PROVIDER_HPP_

#include <pubsub/pubsub_provider.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace fletcher {

/// Transport selection for XRCE-DDS.
enum class XrceTransport { kUdp, kTcp, kSerial };

/// Configuration for the XRCE-DDS provider.
struct XrceConfig {
    XrceTransport transport = XrceTransport::kUdp;
    std::string   agent_ip  = "127.0.0.1";
    uint16_t      agent_port = 2018;

    /// Serial device (only when transport == kSerial).
    std::string   serial_device;
    uint32_t      serial_baudrate = 115200;

    /// Maximum payload size in bytes (default 512 for constrained devices).
    uint32_t      max_payload = 512;

    /// XRCE reliable stream history depth.
    uint16_t      stream_history = 4;

    /// Milliseconds per uxr_run_session_time call in the run-loop thread.
    uint32_t      run_loop_ms = 10;

    /// XRCE session key (must be unique per client on the same agent).
    uint32_t      session_key = 0xAABBCCDD;
};

/// PubSubProvider backed by eProsima Micro XRCE-DDS Client.
///
/// Requires a running XRCE-DDS Agent process (e.g. MicroXRCEAgent).
/// The client communicates with the agent over UDP, TCP, or serial.
///
/// Schema delivery uses a companion "__schema" topic with RELIABLE QoS,
/// matching the FastDDS provider pattern.
class XrceDDSPubSubProvider : public PubSubProvider {
 public:
    explicit XrceDDSPubSubProvider(const XrceConfig& config = {});
    ~XrceDDSPubSubProvider() override;

    XrceDDSPubSubProvider(const XrceDDSPubSubProvider&) = delete;
    XrceDDSPubSubProvider& operator=(const XrceDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments,
                     OwnedSchema schema) override;

    void Publish(const std::vector<std::string>& topic_segments,
                 RowEncoder encoder,
                 const Attachments& attachments = {}) override;

    SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments,
        SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // XRCE_DDS_PUBSUB_PROVIDER_HPP_
