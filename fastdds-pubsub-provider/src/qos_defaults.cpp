// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "internal/qos_defaults.hpp"

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

// max_samples is 100 rather than Fast DDS's 5000: the type is bounded, so a data-sharing writer
// sizes its shared segment at (max_samples + extra_samples) * sizeof(FletcherSample) and reserves
// all of it up front. At 5000 that is gigabytes, which overflows the segment's 32-bit size and
// drops the endpoint back to the transport. 100 is what Fast DDS allocates anyway, and KEEP_ALL
// stays lossless either way.
DataWriterQos MakeFletcherDefaultWriterQos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_ALL_HISTORY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = 100;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 100;
    return qos;
}

DataReaderQos MakeFletcherDefaultReaderQos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_ALL_HISTORY_QOS;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = 100;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 100;

    // Data-sharing is declined on the READ side only; the writer above keeps it, so loan_publish
    // and the zero-copy publish path are unaffected. This costs zero-copy *receive* by default.
    //
    // Why: with data-sharing on both ends, a reader that joins AFTER the rows were published
    // intermittently receives only a subset of the TRANSIENT_LOCAL backlog — often just the newest
    // — with no error anywhere. Measured on integration-tests/gateway-fastdds-ts (Windows,
    // Fast DDS 3.4.0), where the C++ peer publishes three rows before any reader exists:
    //
    //     writer ON  / reader ON   -> 4/4 pass, then 2/4, then 2/4   (1 of 3 rows, or none)
    //     writer ON  / reader OFF  -> 4/4 pass x3                     (this setting)
    //     writer OFF / reader OFF  -> 4/4 pass x3
    //     writer ON  / reader ON, max_samples 8 instead of 100 -> 4/4 pass x3
    //
    // It is not Fletcher dropping them: OrderedDelivery's pre-schema trim logs when it discards,
    // and never fired. The size sensitivity (a 0.5 MB pool is reliable where a 6.6 MB one is not)
    // points below the provider. Note the provider's own suite cannot see any of this — it is
    // single-process, so Fast DDS serves those tests over intra-process delivery, which bypasses
    // data-sharing entirely; the only cross-process coverage is that integration test.
    //
    // Revert this line to re-enable zero-copy receive once the underlying behaviour is understood
    // (it wants an eProsima-level answer, and is the natural home of a future zero-copy-receive
    // round). A caller who wants it today can set data_sharing().automatic() on their own reader
    // QoS — the type stays bounded and plain, so nothing here forecloses it.
    qos.data_sharing().off();
    return qos;
}

// One retained sample for the writer's life, so the pool is pinned to one slot; the defaults
// would reserve 100 of a bounded type per schema endpoint.
DataWriterQos MakeSchemaChannelWriterQos() {
    DataWriterQos qos = DATAWRITER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = 1;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 1;
    qos.resource_limits().allocated_samples = 1;
    return qos;
}

DataReaderQos MakeSchemaChannelReaderQos() {
    DataReaderQos qos = DATAREADER_QOS_DEFAULT;
    qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    qos.history().kind = KEEP_LAST_HISTORY_QOS;
    qos.history().depth = 1;
    qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    // Unkeyed, so one instance holds the whole history.
    qos.resource_limits().max_samples = 1;
    qos.resource_limits().max_instances = 1;
    qos.resource_limits().max_samples_per_instance = 1;
    qos.resource_limits().allocated_samples = 1;
    return qos;
}

}  // namespace internal
}  // namespace fletcher
