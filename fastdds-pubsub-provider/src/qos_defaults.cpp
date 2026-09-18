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
using eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
using eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS;
using eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;

// Fletcher's baked-in document. KEEP_LAST is both the DDS spec's default and Fast DDS's own;
// KEEP_ALL is the exception, kept only for a topic that declares it in its own per-topic profile
// (with TRANSIENT_LOCAL, for replay). Under KEEP_LAST a full history discards the OLDEST sample
// (`WriterHistory::remove_min_change`) and the writer never blocks -- latest-value semantics. A
// reader that falls more than `depth` samples behind gets a GAP and `OnSampleLost` rather than
// costing the publisher a stall.
// depth 25: the unkeyed sample type has one instance, so depth is also the RELIABLE in-flight
// window, and under KEEP_LAST it is depth alone that sizes the writer's pool -- Fast DDS takes the
// pool maximum from the history depth and ignores `max_samples`
// (`DataWriterHistory::to_history_attributes` substitutes depth for it). The type is bounded, so a
// data-sharing writer reserves that whole pool in one shared segment at creation:
// `depth + extra_samples` slots of about (bound + a small header) each, 26 slots and ~1.7 MB at
// the 64 KiB default. `max_samples` is 25 rather than Fast DDS's 5000 for two reasons: the three
// numbers then agree, and `max_samples` IS the pool maximum for a reader whatever its history kind
// (`DataReaderHistory::to_history_attributes` reads it directly) and for a writer whose history is
// KEEP_ALL -- 5000 there is 5001 slots, ~328 MB per endpoint at the 64 KiB bound, and past a
// ~858 KiB bound more than the segment's 32-bit size can address. `allocated_samples` matches both
// so a pool that is not data-sharing is reserved at creation too, not grown into.
// `durability` is VOLATILE at both ends: a topic that needs replay declares TRANSIENT_LOCAL in a
// per-topic profile instead. Nothing here ever blocks a writer, so `max_blocking_time` and
// `heartbeat_period` have no job to do and stay at Fast DDS's own defaults.
// The companion `__schema` channel (below) is RELIABLE + KEEP_LAST(1) + TRANSIENT_LOCAL, one
// retained sample per topic.
const char* FletcherDefaultProfilesDocument() {
    return R"XML(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">
      <rtps><name>FletcherParticipant</name></rtps>
    </participant>
    <data_writer profile_name="default_writer" is_default_profile="true">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability>
          <kind>RELIABLE</kind>
        </reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>25</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>25</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>25</max_samples_per_instance>
          <allocated_samples>25</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_writer>
    <data_reader profile_name="default_reader" is_default_profile="true">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>25</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>25</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>25</max_samples_per_instance>
          <allocated_samples>25</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_reader>
  </profiles>
</dds>)XML";
}

// RELIABLE + KEEP_LAST(1) + TRANSIENT_LOCAL, one retained sample per topic; data-sharing at Fast
// DDS's default.
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
