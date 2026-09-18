// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: Fletcher's baked-in default Fast DDS XML profiles document, plus the companion
// __schema channel's fixed QoS. NOT installed — it lives under src/ because nothing outside this
// provider may name an eProsima type.
//
// An empty `ProviderConfig::document` loads exactly `FletcherDefaultProfilesDocument()` — there is
// exactly one path, so a document supplied by the operator and no document are the same path with
// different bytes. The README publishes this string's exact text as the operator's starting point,
// pinned setting-for-setting by `FastDdsConfig.DefaultProfileTranscriptionIsExact`.

#ifndef FLETCHER_FAST_DDS_SRC_INTERNAL_QOS_DEFAULTS_HPP_
#define FLETCHER_FAST_DDS_SRC_INTERNAL_QOS_DEFAULTS_HPP_

#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>

namespace fletcher {
namespace internal {

/// Fletcher's baked-in Fast DDS XML profiles document — the exact text of the README's "published
/// starting point" block. Every `FastDDSPubSubProvider` built with an empty
/// `ProviderConfig::document` loads exactly this string (fast_dds_pubsub_provider.cpp).
const char* FletcherDefaultProfilesDocument();

// The companion __schema channel: one small retained sample per topic, so KEEP_LAST(1). Fixed
// rather than configurable — it is an implementation detail of how the schema reaches a subscriber
// that joined late.
eprosima::fastdds::dds::DataWriterQos MakeSchemaChannelWriterQos();
eprosima::fastdds::dds::DataReaderQos MakeSchemaChannelReaderQos();

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FAST_DDS_SRC_INTERNAL_QOS_DEFAULTS_HPP_
