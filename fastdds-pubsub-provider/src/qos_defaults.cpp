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

// Fletcher's baked-in document (owner decision 2026-09-15 — replaces the hand-built
// MakeFletcherDefaultData{Writer,Reader}Qos() this round retired). `max_samples` is 100 rather than
// Fast DDS's 5000: the type is bounded, so a data-sharing writer sizes its shared segment at
// (max_samples + extra_samples) * sizeof(FletcherSample) and reserves all of it up front, and 5000
// overflows the segment's 32-bit size. `heartbeat_period` 20 ms: measured (after-e2e.txt, affinity
// 0xC), a writer blocked on a full KEEP_ALL history sends no heartbeats but the periodic one (3 s
// default), which exceeds a 100 ms `max_blocking_time` and drops the sample. `max_blocking_time`
// DURATION_INFINITY: cdb-reproduced (item I), a 100 ms timeout still drops a sample under a
// sustained stall, and that drop leaves a sequence-number gap a RELIABLE reader refuses to see
// past — infinite blocks the writer instead, bounded only by the participant's 20 s lease for a
// truly dead peer.
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
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
          <allocated_samples>100</allocated_samples>
        </resourceLimitsQos>
      </topic>
      <times>
        <heartbeat_period>
          <sec>0</sec>
          <nanosec>20000000</nanosec>
        </heartbeat_period>
      </times>
    </data_writer>
    <data_reader profile_name="default_reader" is_default_profile="true">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
          <allocated_samples>100</allocated_samples>
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
