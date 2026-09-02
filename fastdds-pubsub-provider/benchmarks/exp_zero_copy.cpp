// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Ping-pong latency across the two axes that make a sample zero-copy: loaning, and data-sharing.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fastdds/LibrarySettings.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace fletcher;
using namespace eprosima::fastdds::dds;

namespace {

constexpr int kWarmup = 500;
constexpr int kIters = 5000;

OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

// PDA-DEC-6 moved every knob below out of the retired `FastDDSProviderOptions` and into the
// provider's own Fast DDS XML profiles document. This function is the item's own proof that the
// document expresses what the struct did — resource limits, data-sharing, and the publish path —
// and it is the only in-tree caller of `fletcher.loan_publish`.
//
// The durability / reliability / history lines are restated in full because **a supplied profile
// is that endpoint's WHOLE quality-of-service** (owner ruling 2026-09-02): Fletcher's built-in
// profile is not underneath it, so a profile that mentioned only `resourceLimitsQos` would take
// Fast DDS's defaults for everything else — including a BEST_EFFORT reader, which would measure
// something other than what this benchmark is about.
std::string Document(bool loan, bool sharing, int32_t slots) {
    const std::string limits = R"(
        <historyQos><kind>KEEP_LAST</kind><depth>)" +
                               std::to_string(slots) + R"(</depth></historyQos>
        <resourceLimitsQos>
          <max_samples>)" + std::to_string(slots) +
                               R"(</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>)" +
                               std::to_string(slots) + R"(</max_samples_per_instance>
          <allocated_samples>)" +
                               std::to_string(slots) +
                               R"(</allocated_samples>
        </resourceLimitsQos>)";
    // AUTO is Fast DDS's own default, so "sharing" is the absence of an OFF line.
    const std::string data_sharing = sharing ? "" : R"(
        <data_sharing><kind>OFF</kind></data_sharing>)";
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">
      <rtps>
        <propertiesPolicy>
          <properties>
            <property>
              <name>fletcher.loan_publish</name>
              <value>)" +
           std::string(loan ? "true" : "false") +
           R"(</value>
            </property>
          </properties>
        </propertiesPolicy>
      </rtps>
    </participant>
    <data_writer profile_name="fletcher_writer">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>)" +
           data_sharing + R"(
      </qos>
      <topic>)" +
           limits + R"(
      </topic>
    </data_writer>
    <data_reader profile_name="fletcher_reader">
      <qos>
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>)" +
           data_sharing + R"(
      </qos>
      <topic>)" +
           limits + R"(
      </topic>
    </data_reader>
  </profiles>
</dds>)";
}

ProviderConfig Options(bool loan, bool sharing, uint32_t bound, int32_t slots) {
    ProviderConfig config;
    config.domain_id = 43;
    config.max_payload_bytes = bound;
    config.document = Document(loan, sharing, slots);
    return config;
}

double Percentile(std::vector<double>& v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

// Publish cost with no reader matched: the writer path alone.
void PublishOnly(const char* label, bool loan, bool sharing, size_t row_bytes, uint32_t bound,
                 int32_t slots) {
    FastDDSPubSubProvider pub(Options(loan, sharing, bound, slots));
    const std::vector<std::string> topic{"solo", label};
    pub.CreateTopic(topic, MakeSchema());
    const std::vector<uint8_t> row(row_bytes, 0xAB);
    const auto encoder = [&row](WriteBuffer& buf) { buf.Append(row.data(), row.size()); };
    try {
        for (int i = 0; i < kWarmup; ++i) pub.Publish(topic, encoder);
    } catch (const std::exception& e) {
        std::printf("  %-26s THREW %s\n", label, e.what());
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i) pub.Publish(topic, encoder);
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() /
        kIters;
    std::printf("  %-26s publish-only %8.0f ns\n", label, ns);
}

// One sample in flight at a time, so this is latency and not throughput.
void PingPong(const char* label, bool loan, bool sharing, size_t row_bytes, uint32_t bound,
              int32_t slots) {
    FastDDSPubSubProvider pub(Options(loan, sharing, bound, slots));
    FastDDSPubSubProvider sub(Options(loan, sharing, bound, slots));
    const std::vector<std::string> topic{"pp", label};
    pub.CreateTopic(topic, MakeSchema());

    std::atomic<uint64_t> arrived{0};
    auto result = sub.Subscribe(
        topic, [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            uint64_t seq = 0;
            if (len >= sizeof(seq)) std::memcpy(&seq, data, sizeof(seq));
            arrived.store(seq, std::memory_order_release);
        });
    SharedSchema topic_schema;
    if (result.schema.Wait(std::chrono::seconds(5), &topic_schema) != PubSubStatus::kOk ||
        !topic_schema) {
        std::printf("  %-26s SCHEMA HANDOFF FAILED\n", label);
        return;
    }

    std::vector<uint8_t> row(std::max(row_bytes, sizeof(uint64_t)), 0xAB);
    uint64_t seq = 0;
    const auto encoder = [&row](WriteBuffer& buf) { buf.Append(row.data(), row.size()); };

    std::vector<double> samples;
    samples.reserve(kIters);
    for (int i = 0; i < kWarmup + kIters; ++i) {
        ++seq;
        std::memcpy(row.data(), &seq, sizeof(seq));
        const auto t0 = std::chrono::steady_clock::now();
        try {
            pub.Publish(topic, encoder);
        } catch (const std::exception& e) {
            std::printf("  %-26s THREW %s\n", label, e.what());
            return;
        }
        const auto deadline = t0 + std::chrono::seconds(2);
        while (arrived.load(std::memory_order_acquire) != seq &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const auto t1 = std::chrono::steady_clock::now();
        if (arrived.load(std::memory_order_acquire) != seq) {
            std::printf("  %-26s LOST at %d\n", label, i);
            return;
        }
        if (i >= kWarmup) {
            samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        }
    }
    std::printf("  %-26s p50 %8.0f ns   p99 %8.0f ns\n", label, Percentile(samples, 0.50),
                Percentile(samples, 0.99));
}

struct Combo {
    const char* name;
    bool loan;
    bool sharing;
};

}  // namespace

int main() {
    std::setbuf(stdout, nullptr);
    // Intra-process delivery defaults to INTRAPROCESS_FULL, which short-circuits writer to reader
    // inside one process and bypasses both data-sharing and the transport. Off, so the paths under
    // test are the ones actually measured.
    eprosima::fastdds::LibrarySettings settings;
    DomainParticipantFactory::get_instance()->get_library_settings(settings);
    settings.intraprocess_delivery = eprosima::fastdds::INTRAPROCESS_OFF;
    if (DomainParticipantFactory::get_instance()->set_library_settings(settings) != RETCODE_OK) {
        std::printf("could not disable intra-process delivery\n");
        return 1;
    }
    struct Case {
        const char* what;
        size_t row;
        uint32_t bound;
        int32_t slots;
    };

    // A big bound times a big history is gigabytes of preallocation, so slots shrink as the bound
    // grows. 16 slots of 512 KiB is 8 MB per endpoint.
    const Case cases[] = {
        {"row 512 KiB - 64, bound 512 KiB", 512u * 1024 - 64, 512u * 1024, 16},
        {"row 214 B,   bound 512 KiB", 214, 512u * 1024, 16},
        {"row 128 KiB - 64, bound 128 KiB", 128u * 1024 - 64, 128u * 1024, 32},
    };

    for (const Case& k : cases) {
        std::printf("%s== %s ==%s", "\n", k.what, "\n");
        for (const Combo& c :
             {Combo{"loan=on  sharing=on ", true, true}, Combo{"loan=off sharing=on ", false, true},
              Combo{"loan=on  sharing=off", true, false},
              Combo{"loan=off sharing=off", false, false}}) {
            PublishOnly(c.name, c.loan, c.sharing, k.row, k.bound, k.slots);
            PingPong(c.name, c.loan, c.sharing, k.row, k.bound, k.slots);
        }
    }
    return 0;
}
