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
// per-topic profile instead. The default pair never blocks a writer, so it leaves
// `max_blocking_time` and `heartbeat_period` at Fast DDS's own defaults; `lossless` below sets
// both.
// The companion `__schema` channel (below) is RELIABLE + KEEP_LAST(1) + TRANSIENT_LOCAL, one
// retained sample per topic.
//
// Five named pairs sit beside `default_writer`/`default_reader`, each a `<data_writer>` and a
// `<data_reader>` profile sharing one name, so `{.profile = "name"}` on both `CreateTopic` and
// `Subscribe` resolves a matched writer and reader (writer and reader profile names live in
// separate registry maps, so the same name picks the right side on each call rather than
// colliding): `fire_and_forget` (BEST_EFFORT, VOLATILE, KEEP_LAST 1) drops a lagging sample rather
// than retransmit it; `latest` (RELIABLE, VOLATILE, KEEP_LAST 1) newest value only, resent if
// lost, no replay; `store_latest` (RELIABLE, TRANSIENT_LOCAL, KEEP_LAST 1) replays the last value
// to a late subscriber; `store_history` (RELIABLE, TRANSIENT_LOCAL, KEEP_LAST 25) replays the last
// 25; `lossless` (RELIABLE, VOLATILE, KEEP_ALL, an infinite `max_blocking_time`, 20 ms heartbeat)
// delivers every sample in order and blocks the writer rather than drop one.
// `default_writer`/`default_reader` are this file's own "stream" semantics -- RELIABLE, VOLATILE,
// KEEP_LAST 25 -- and are what every topic without a profile of its own runs on.
const char* FletcherDefaultProfilesDocument() {
    return R"XML(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">
      <rtps><name>FletcherParticipant</name></rtps>
    </participant>
    <!-- newest value only, no retransmission: high-rate sensor streams where a lost sample is
         replaced by the next one -->
    <data_writer profile_name="fire_and_forget">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>BEST_EFFORT</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>1</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>1</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>1</max_samples_per_instance>
          <allocated_samples>1</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_writer>
    <data_reader profile_name="fire_and_forget">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>BEST_EFFORT</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>1</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>1</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>1</max_samples_per_instance>
          <allocated_samples>1</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_reader>
    <!-- the newest value, retransmitted if lost, nothing replayed to a late subscriber: a live
         state that must not be missed while it is live -->
    <data_writer profile_name="latest">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>1</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>1</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>1</max_samples_per_instance>
          <allocated_samples>1</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_writer>
    <data_reader profile_name="latest">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>1</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>1</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>1</max_samples_per_instance>
          <allocated_samples>1</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_reader>
    <!-- the last value, replayed to a late subscriber: state, configuration, status -->
    <data_writer profile_name="store_latest">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>1</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>1</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>1</max_samples_per_instance>
          <allocated_samples>1</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_writer>
    <data_reader profile_name="store_latest">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos>
          <kind>KEEP_LAST</kind>
          <depth>1</depth>
        </historyQos>
        <resourceLimitsQos>
          <max_samples>1</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>1</max_samples_per_instance>
          <allocated_samples>1</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_reader>
    <!-- the last 25 values, replayed to a late subscriber: track tails, recent events -->
    <data_writer profile_name="store_history">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
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
    </data_writer>
    <data_reader profile_name="store_history">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
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
    <!-- every sample, in order: commands, events, logs. A reader that stops taking samples stalls
         the publisher; a crashed peer frees it when its participant lease expires (20 s by
         default); the slowest reader can hold the publisher back once 25 samples are in flight -->
    <data_writer profile_name="lossless">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability>
          <kind>RELIABLE</kind>
          <max_blocking_time>DURATION_INFINITY</max_blocking_time>
        </reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>25</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>25</max_samples_per_instance>
          <allocated_samples>25</allocated_samples>
        </resourceLimitsQos>
      </topic>
      <times><heartbeat_period><sec>0</sec><nanosec>20000000</nanosec></heartbeat_period></times>
    </data_writer>
    <data_reader profile_name="lossless">
      <qos>
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>25</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>25</max_samples_per_instance>
          <allocated_samples>25</allocated_samples>
        </resourceLimitsQos>
      </topic>
    </data_reader>
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
