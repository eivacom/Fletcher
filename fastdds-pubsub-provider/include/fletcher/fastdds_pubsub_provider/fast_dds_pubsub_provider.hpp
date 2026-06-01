// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <cstdint>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "fletcher/fastdds_pubsub_provider/internal/qos_defaults.hpp"

namespace fletcher {

/// Typed configuration for FastDDSPubSubProvider. All QoS settings
/// are specified up-front; the provider is immutable with respect to
/// configuration after construction.
///
/// Per-topic QoS overrides are keyed by the joined topic string
/// ("a/b/c"). The provider looks up overrides first, then falls back
/// to the instance defaults. The default values for default_writer_qos
/// and default_reader_qos encode Fletcher's profile — see the
/// fastdds-pubsub-provider README for what that profile contains.
struct FastDDSProviderOptions {
    /// DDS domain ID.
    uint32_t domain_id = 0;

    /// Maximum DDS payload size in bytes. Bounds the full serialised
    /// envelope: CDR framing + row bytes + all attachments. Also
    /// applied to the companion schema channel.
    uint32_t max_payload_bytes = 1024 * 1024;

    /// Default DataWriter QoS applied to any topic without a per-topic
    /// override.
    eprosima::fastdds::dds::DataWriterQos default_writer_qos =
        internal::MakeFletcherDefaultWriterQos();

    /// Default DataReader QoS applied to any topic without a per-topic
    /// override.
    eprosima::fastdds::dds::DataReaderQos default_reader_qos =
        internal::MakeFletcherDefaultReaderQos();

    /// Per-topic DataWriter QoS overrides. Key: joined topic string.
    std::unordered_map<std::string, eprosima::fastdds::dds::DataWriterQos> topic_writer_qos;

    /// Per-topic DataReader QoS overrides. Key: joined topic string.
    std::unordered_map<std::string, eprosima::fastdds::dds::DataReaderQos> topic_reader_qos;
};

/// PubSubProvider transport backed by eProsima Fast DDS.
///
/// QoS configuration is supplied entirely via FastDDSProviderOptions
/// at construction time. There are no runtime setters: this avoids
/// timing bugs (e.g. setting QoS after the DataWriter has already
/// been created) and keeps the provider's internal state immutable
/// after construction.
///
/// The companion schema channel (__schema topic) always uses RELIABLE +
/// KEEP_LAST(depth=1) + TRANSIENT_LOCAL and is not configurable —
/// Fletcher-internal implementation detail.
class FastDDSPubSubProvider : public PubSubProvider {
   public:
    explicit FastDDSPubSubProvider(FastDDSProviderOptions options);
    ~FastDDSPubSubProvider() override;

    FastDDSPubSubProvider(const FastDDSPubSubProvider&) = delete;
    FastDDSPubSubProvider& operator=(const FastDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema) override;

    void Publish(const std::vector<std::string>& topic_segments, RowEncoder encoder,
                 const Attachments& attachments = {}) override;

    SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                 SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
