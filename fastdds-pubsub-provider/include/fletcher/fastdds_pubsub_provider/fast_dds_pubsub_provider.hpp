// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <cstdint>
#include <fletcher/pubsub/pubsub.hpp>
#include <memory>

namespace fletcher {

/// PubSub transport backed by eProsima Fast DDS.
///
/// Default data topic QoS (DataWriter and DataReader): RELIABLE reliability,
/// KEEP_ALL history, and TRANSIENT_LOCAL durability.  Callers can override
/// per-topic by passing a DataWriterQos to CreateTopic or a DataReaderQos
/// to Subscribe via the std::any config parameter.
///
/// The companion schema channel (__schema topic) uses RELIABLE +
/// KEEP_LAST(depth=1) + TRANSIENT_LOCAL to retain only the latest schema.
class FastDDSPubSubProvider : public PubSub {
   public:
    /// @param domain_id         DDS domain ID (default 0).
    /// @param max_payload_bytes Maximum DDS payload size in bytes (default 1 MB).
    ///                          Bounds the full serialized envelope: CDR framing +
    ///                          row bytes + all attachments. Also applied to the
    ///                          companion schema channel.
    explicit FastDDSPubSubProvider(uint32_t domain_id = 0,
                                   uint32_t max_payload_bytes = 1024 * 1024);
    ~FastDDSPubSubProvider() override;

    FastDDSPubSubProvider(const FastDDSPubSubProvider&) = delete;
    FastDDSPubSubProvider& operator=(const FastDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema,
                     std::any config = {}) override;

    void Publish(const std::vector<std::string>& topic_segments, RowEncoder encoder,
                 const Attachments& attachments = {}) override;

    SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                 SubscribeCallback callback, std::any config = {}) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
