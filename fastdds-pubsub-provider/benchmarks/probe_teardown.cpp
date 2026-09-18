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
//
// The single-process shapes above never provoke the spin: Fast DDS delivers same-process
// endpoints intraprocess, so the reader's DataSharingListener never sees a payload and
// process_data_msg is never in the picture. `probe_teardown pub [seconds]` and `probe_teardown
// sub [cycles]` split provider A and provider B across two processes on the same host and the
// same domain (44) and topic, so data-sharing AUTOMATIC actually engages between them and the
// listener thread is live for `sub`'s Subscribe/Unsubscribe cycles to tear down against.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <iostream>
#include <memory>
#include <mutex>
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

// Provider A only: publish the 198 B row flat out on its own thread, for `seconds` or until
// stdin closes, whichever first, ignoring whatever Publish reports (this probe is about a
// reader's teardown, not publish-side loss).
int RunPub(int seconds) {
    ProviderConfig config;
    config.domain_id = 44;
    auto a = std::make_unique<FastDDSPubSubProvider>(config);
    a->CreateTopic({"probe", "teardown"}, MakeSchema());

    std::mutex mu;
    std::condition_variable cv;
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

    std::printf("pub started\n");

    // Detached, not joined: if `seconds` elapses first this thread is still blocked in
    // std::getline, and process exit tears it down. Notifies the same cv the deadline wait
    // below blocks on, so closing stdin can end the run early without a polling sleep.
    std::thread([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
        }
        {
            std::lock_guard<std::mutex> lock(mu);
            stop_publishing.store(true, std::memory_order_relaxed);
        }
        cv.notify_all();
    }).detach();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_until(lock, deadline,
                      [&] { return stop_publishing.load(std::memory_order_relaxed); });
    }
    stop_publishing.store(true, std::memory_order_relaxed);
    publisher.join();

    std::printf("pub done\n");
    return 0;
}

// Provider B only: `cycles` of Subscribe / wait for >= 50 rows or 2 s / Unsubscribe against
// whatever is publishing on the same domain and topic, then destroy B.
int RunSub(int cycles) {
    ProviderConfig config;
    config.domain_id = 44;
    auto b = std::make_unique<FastDDSPubSubProvider>(config);

    for (int cycle = 0; cycle < cycles; ++cycle) {
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<int> received{0};
        SubscriptionResult result =
            b->Subscribe({"probe", "teardown"},
                         [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                             received.fetch_add(1, std::memory_order_relaxed);
                             std::lock_guard<std::mutex> lock(mu);
                             cv.notify_all();
                         });
        SharedSchema schema;
        result.schema.Wait(std::chrono::seconds(2), &schema);

        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::seconds(2),
                        [&] { return received.load(std::memory_order_relaxed) >= 50; });
        }

        b->Unsubscribe({"probe", "teardown"});

        if ((cycle + 1) % 10 == 0) {
            std::printf("cycle=%d rows=%d\n", cycle + 1, received.load(std::memory_order_relaxed));
        }
    }

    b.reset();
    std::printf("done cycles=%d\n", cycles);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);

    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "pub") return RunPub(argc > 2 ? std::atoi(argv[2]) : 120);
    if (mode == "sub") return RunSub(argc > 2 ? std::atoi(argv[2]) : 300);

    const bool control = mode == "off";
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
        std::mutex mu;
        std::condition_variable cv;
        std::atomic<int> received{0};
        SubscriptionResult result =
            b->Subscribe({"probe", "teardown"},
                         [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                             received.fetch_add(1, std::memory_order_relaxed);
                             std::lock_guard<std::mutex> lock(mu);
                             cv.notify_all();
                         });
        SharedSchema schema;
        result.schema.Wait(std::chrono::seconds(2), &schema);

        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, std::chrono::seconds(2),
                        [&] { return received.load(std::memory_order_relaxed) >= 50; });
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
