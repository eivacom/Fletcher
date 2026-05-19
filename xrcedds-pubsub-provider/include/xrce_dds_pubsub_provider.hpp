// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_XRCE_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_XRCE_DDS_PUBSUB_PROVIDER_HPP_

#include <pubsub/pubsub.hpp>

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

    /// Budget in milliseconds for the initial session creation handshake.
    /// The client retries in ~1000 ms increments, so effective granularity
    /// is one second. Set to <= 1000 in tests to limit to a single attempt.
    int           connect_timeout_ms = 3000;

    /// XRCE reliable stream history depth.
    uint16_t      stream_history = 4;

    /// Milliseconds per uxr_run_session_time call in the run-loop thread.
    uint32_t      run_loop_ms = 10;

    /// XRCE session key (must be unique per client on the same agent).
    uint32_t      session_key = 0xAABBCCDD;

    /// DDS domain id on which the Agent creates this client's
    /// participant. Must match the domain id used by any DDS peers
    /// (e.g. a FastDDS subscriber) that this client wants to talk to.
    uint16_t      domain_id = 0;
};

/// PubSub transport backed by eProsima Micro XRCE-DDS Client.
///
/// Requires a running XRCE-DDS Agent process (e.g. MicroXRCEAgent).
/// The client communicates with the agent over UDP, TCP, or serial.
///
/// Schema delivery uses a companion "__schema" topic with RELIABLE QoS,
/// matching the FastDDS provider pattern.
class XrceDDSPubSubProvider : public PubSub {
 public:
    explicit XrceDDSPubSubProvider(const XrceConfig& config = {});
    ~XrceDDSPubSubProvider() override;

    XrceDDSPubSubProvider(const XrceDDSPubSubProvider&) = delete;
    XrceDDSPubSubProvider& operator=(const XrceDDSPubSubProvider&) = delete;

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

#endif  // FLETCHER_INCLUDE_XRCE_DDS_PUBSUB_PROVIDER_HPP_
