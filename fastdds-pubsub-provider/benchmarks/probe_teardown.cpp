// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Probe, not a benchmark: with the data reader's data_sharing AUTOMATIC (item D,
// qos_defaults.cpp), every data reader now owns a Fast DDS DataSharingListener thread. Does
// repeated Subscribe/Unsubscribe against a flat-out publisher reproduce the Fast DDS 3.4.0
// teardown spin the __schema channel is already known to hit -- StatefulReader::~StatefulReader
// clears is_alive_ before DataSharingListener::stop(), and DataSharingListener::process_new_data
// only advances its pool cursor when process_data_msg succeeds and never checks is_running_, so a
// payload still pending at teardown spins that thread forever and stop()'s join never returns
// (qos_defaults.cpp's comment on MakeSchemaChannelReaderQos) -- no longer confined to that one
// channel?
//
// Provider A publishes a 198 B row on "probe/teardown" flat out, on its own thread, ignoring
// whatever Publish reports (this probe is about teardown, not publish-side loss). Provider B, on
// the main thread, runs 100 cycles of Subscribe / wait for >= 50 rows or 2 s / Unsubscribe, then B
// is destroyed, A's publishing thread is stopped, then A is destroyed. Prints one line per 10
// cycles and "done cycles=100" at the end; stdout is unbuffered so the last line printed is visible
// even if the process is later killed for hanging.
//
// `probe_teardown off` runs the control: a document (BoundedConfig-style, tests/test_fast_dds_...)
// that is Fletcher's built-in in every other respect but forces the DATA reader's data_sharing
// OFF, on an `is_default_profile="true"` `fletcher_reader` -- the setting item D moved to
// AUTOMATIC. No argument (or any other argument) runs Fletcher's built-in as-is.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fletcher;

namespace {

OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

// Mirrors Fletcher's built-in (qos_defaults.cpp) setting for setting, except the data reader's
// data_sharing, forced OFF here instead of left AUTOMATIC -- the one variable this control
// isolates.
const char* kDataSharingOffDocument = R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant"/>
    <data_writer profile_name="fletcher_writer" is_default_profile="true">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
        </resourceLimitsQos>
      </topic>
    </data_writer>
    <data_reader profile_name="fletcher_reader" is_default_profile="true">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
        <data_sharing><kind>OFF</kind></data_sharing>
      </qos>
      <topic>
        <historyQos><kind>KEEP_ALL</kind></historyQos>
        <resourceLimitsQos>
          <max_samples>100</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>100</max_samples_per_instance>
        </resourceLimitsQos>
      </topic>
    </data_reader>
  </profiles>
</dds>)";

}  // namespace

int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);

    const bool control = argc > 1 && std::string(argv[1]) == "off";
    std::printf("mode=%s\n", control ? "data_sharing_off_control" : "data_sharing_automatic");

    ProviderConfig config;
    config.domain_id = 44;
    if (control) config.document = kDataSharingOffDocument;

    auto a = std::make_unique<FastDDSPubSubProvider>(config);
    a->CreateTopic({"probe", "teardown"}, MakeSchema());

    std::atomic<bool> stop_publishing{false};
    const std::vector<uint8_t> row(198, 0xAB);
    const auto encoder = [&row](WriteBuffer& buf) { buf.Append(row.data(), row.size()); };
    std::thread publisher([&] {
        while (!stop_publishing.load(std::memory_order_relaxed)) {
            try {
                a->Publish({"probe", "teardown"}, encoder);
            } catch (...) {
                // Ignored: a drop or a transport error is not what this probe is measuring.
            }
        }
    });

    auto b = std::make_unique<FastDDSPubSubProvider>(config);
    constexpr int kCycles = 100;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        std::atomic<int> received{0};
        SubscriptionResult result =
            b->Subscribe({"probe", "teardown"},
                         [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                             received.fetch_add(1, std::memory_order_relaxed);
                         });
        SharedSchema schema;
        result.schema.Wait(std::chrono::seconds(2), &schema);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (received.load(std::memory_order_relaxed) < 50 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        b->Unsubscribe({"probe", "teardown"});

        if ((cycle + 1) % 10 == 0) {
            std::printf("cycle=%d rows=%d\n", cycle + 1, received.load(std::memory_order_relaxed));
        }
    }

    // Exactly the order the probe is asking about: destroy B (every data reader it ever built torn
    // down here), stop A's publishing thread, then destroy A.
    b.reset();
    stop_publishing.store(true, std::memory_order_relaxed);
    publisher.join();
    a.reset();

    std::printf("done cycles=%d\n", kCycles);
    return 0;
}
