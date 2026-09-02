// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: Fletcher's default QoS profile for FastDDS data topics. NOT installed — it lives
// under src/ because nothing outside this provider may name an eProsima type (PDA-DEC-6 §5).
//
// These are the built-ins a role falls back to when the profiles document names no profile for
// it. They are NOT a floor under a supplied profile: a resolved profile is that endpoint's WHOLE
// QoS (owner ruling 2026-09-02), so these are only ever used INSTEAD of a document profile, never
// merged underneath one. The README publishes their exact XML transcription as the operator's
// starting point, pinned setting-for-setting by
// `FastDdsConfig.DefaultProfileTranscriptionIsExact`.

#ifndef FLETCHER_FAST_DDS_SRC_INTERNAL_QOS_DEFAULTS_HPP_
#define FLETCHER_FAST_DDS_SRC_INTERNAL_QOS_DEFAULTS_HPP_

#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>

namespace fletcher {
namespace internal {

eprosima::fastdds::dds::DataWriterQos MakeFletcherDefaultWriterQos();
eprosima::fastdds::dds::DataReaderQos MakeFletcherDefaultReaderQos();

// The companion __schema channel: one small retained sample per topic, so KEEP_LAST(1). Fixed
// rather than configurable — it is an implementation detail of how the schema reaches a subscriber
// that joined late.
eprosima::fastdds::dds::DataWriterQos MakeSchemaChannelWriterQos();
eprosima::fastdds::dds::DataReaderQos MakeSchemaChannelReaderQos();

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FAST_DDS_SRC_INTERNAL_QOS_DEFAULTS_HPP_
