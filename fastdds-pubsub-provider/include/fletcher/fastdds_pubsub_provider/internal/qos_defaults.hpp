// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: Fletcher's default QoS profile for FastDDS data topics.
// Not public API — used only to provide default-member values for
// FastDDSProviderOptions. The README documents what the defaults are
// in human terms; callers who want to customise should construct or
// modify a DataWriterQos / DataReaderQos directly.

#ifndef FLETCHER_INCLUDE_FAST_DDS_INTERNAL_QOS_DEFAULTS_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_INTERNAL_QOS_DEFAULTS_HPP_

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

#endif  // FLETCHER_INCLUDE_FAST_DDS_INTERNAL_QOS_DEFAULTS_HPP_
