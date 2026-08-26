// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/fastdds_pubsub_provider/internal/qos_defaults.hpp"

#include <cstdint>

namespace fletcher {
namespace internal {

using eprosima::fastdds::dds::DATAREADER_QOS_DEFAULT;
using eprosima::fastdds::dds::DataReaderQos;
using eprosima::fastdds::dds::DATAWRITER_QOS_DEFAULT;
using eprosima::fastdds::dds::DataWriterQos;
using eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS;
using eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
using eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
using eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;

namespace {

// Fast DDS defaults max_samples to 5000 while allocating 100. That is harmless for a heap payload
// pool, which grows on demand, but the type is bounded, so a data-sharing writer sizes its shared
// segment at (max_samples + extra_samples) * sizeof(FletcherSample) and allocates all of it up
// front — gigabytes, which overflows the segment's 32-bit size and drops the endpoint back to the
// transport. Capping max_samples at what is actually reserved keeps KEEP_ALL's lossless semantics
// and makes the segment the size it looks like it should be.
constexpr int32_t kMaxSamples = 100;

// The channel holds one sample for the writer's life; the defaults would reserve 100.
constexpr int32_t kSchemaSamples = 1;

}  // namespace

DataWriterQos MakeFletcherDefaultWriterQos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_ALL_HISTORY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = kMaxSamples;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = kMaxSamples;
    return qos;
}

DataReaderQos MakeFletcherDefaultReaderQos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_ALL_HISTORY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = kMaxSamples;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = kMaxSamples;
    return qos;
}

DataWriterQos MakeSchemaChannelWriterQos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = kSchemaSamples;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = kSchemaSamples;
    qos.resource_limits().allocated_samples = kSchemaSamples;
    return qos;
}

DataReaderQos MakeSchemaChannelReaderQos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = kSchemaSamples;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = kSchemaSamples;
    qos.resource_limits().allocated_samples = kSchemaSamples;
    return qos;
}

}  // namespace internal
}  // namespace fletcher
