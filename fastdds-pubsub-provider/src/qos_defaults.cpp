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

    // Measured this round (after-e2e.txt, affinity 0xC): a flat-out publisher against an
    // asynchronous reader fills KEEP_ALL history (100), the writer blocks in write(), and a
    // blocked writer sends no heartbeats but the periodic one (3 s default) -- recovery then
    // exceeds max_blocking_time (100 ms), so write() times out and the sample is dropped
    // ("[FLETCHER_PUBLICATION Error] ... return code 10"). 20 ms makes a matched, un-acked reader
    // re-synced well inside 100 ms. Idle cost: one heartbeat per matched reader per 20 ms.
    qos.reliable_writer_qos().times.heartbeat_period =
        eprosima::fastdds::dds::Duration_t(0, 20'000'000);

    // cdb-reproduced this round (item I): the 100 ms timeout above still drops a sample under a
    // sustained stall, and that drop leaves a sequence-number gap a RELIABLE reader then refuses
    // to see past (ReaderHistory::can_change_be_added_nts) -- every later write times out too, for
    // the rest of the run. Infinite: a KEEP_ALL RELIABLE writer blocks until the reader frees space
    // instead (this is what the README already promises); a dead peer is bounded by the
    // participant's 20 s lease, not by this.
    qos.reliability().max_blocking_time = eprosima::fastdds::dds::c_TimeInfinite;
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

    // AUTOMATIC on both ends (Fast DDS's own default), owner decision 2026-09-14: the earlier
    // measured late-joiner backlog loss (see the README's history for the OFF-era numbers) is
    // re-verified cross-process by integration-tests/gateway-fastdds-ts, 3x, as part of this round.
    // The __schema channel keeps data-sharing OFF regardless (MakeSchemaChannel*Qos below): its own
    // 3.4.0 teardown hang is unrelated to this setting.
    return qos;
}

// The schema channel declines data-sharing on both ends. Its type is the same plain
// FletcherSamplePubSubType the data channel uses (registered as SchemaBytesPubSubType), so this is
// not a plainness question — it is a one-sample control channel. OFF removes a DataSharingListener
// thread per schema reader and a shared-memory segment per schema writer, and it sidesteps a Fast
// DDS 3.4.0 teardown hang: StatefulReader::~StatefulReader clears is_alive_ before
// DataSharingListener::stop(), and DataSharingListener::process_new_data only advances its pool
// cursor when process_data_msg succeeds and never checks is_running_, so a payload still pending at
// teardown spins that thread forever and stop()'s join never returns. Reproduced by
// ConflictingCrossProviderSchemaIsLoggedNotSwallowed.
//
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
    qos.data_sharing().off();
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
    qos.data_sharing().off();
    return qos;
}

}  // namespace internal
}  // namespace fletcher
