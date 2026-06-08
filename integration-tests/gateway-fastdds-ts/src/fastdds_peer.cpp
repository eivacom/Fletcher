// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// fastdds_peer — a small C++ FastDDS application used by the
// gateway-fastdds-ts integration test. It exercises both directions of the
// gateway's FastDDS provider using protoc-gen-fletcher generated types:
//
//   * Publishes a known set of rows on the CppToTs topic (a TS client
//     subscribing through the gateway receives them).
//   * Subscribes to the TsToCpp topic and prints every received row to
//     stdout as a "RECV ..." line, so the vitest orchestrator can assert
//     what a TS publisher pushed through the gateway over DDS.
//
// Process lifecycle mirrors the gateway exe: prints "READY" once both
// endpoints are set up, and exits cleanly on the literal stdin line "stop".
//
// CLI:
//   --domain-id N   DDS domain id to join (default 0). Must match the
//                   gateway's --domain-id.

#include <cstdint>
#include <cstdio>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "sensor_reading.fletcher.pb.h"

namespace {

uint32_t ParseDomainId(int argc, char* argv[]) {
    uint32_t domain_id = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--domain-id" && i + 1 < argc) {
            domain_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        }
    }
    return domain_id;
}

}  // namespace

int main(int argc, char* argv[]) {
    const uint32_t domain_id = ParseDomainId(argc, argv);

    fletcher::FastDDSProviderOptions options;
    options.domain_id = domain_id;
    std::shared_ptr<fletcher::FastDDSPubSubProvider> provider =
        std::make_shared<fletcher::FastDDSPubSubProvider>(std::move(options));

    // C++ -> TS : the generated publisher's constructor calls CreateTopic,
    // which announces the CppToTs schema on the DDS __schema channel so the
    // gateway's late-joining DataReader can decode the rows published below.
    fletcher_gen::gwfastdds::SensorFeed_CppToTsPublisher cpp_to_ts(provider);

    // TS -> C++ : subscribe and echo each received row to stdout. Subscribe is
    // non-blocking and never throws when no publisher exists yet; the schema
    // (and any rows buffered until it arrives) are delivered once the TS client
    // publishes through the gateway, so no local placeholder publisher for
    // TsToCpp is needed.
    fletcher_gen::gwfastdds::SensorFeed_TsToCppSubscriber ts_to_cpp(provider);
    ts_to_cpp.Subscribe([](fletcher_gen::gwfastdds::SensorReading row, fletcher::Attachments) {
        std::printf("RECV sensor_id=%d value=%g unit=%s\n", row.sensor_id(), row.value(),
                    std::string(row.unit()).c_str());
        std::fflush(stdout);
    });

    // C++ -> TS : publish a known set of rows on CppToTs. RELIABLE + KEEP_ALL +
    // TRANSIENT_LOCAL retains them, so the gateway's late-joining DataReader
    // (created when a TS client subscribes) still receives every row.
    const int row_count = 3;
    for (int i = 0; i < row_count; ++i) {
        fletcher_gen::gwfastdds::SensorReading row;
        row.set_sensor_id(i + 1).set_value((i + 1) * 10.5).set_unit("u" + std::to_string(i + 1));
        cpp_to_ts.Publish(row);
    }

    std::printf("READY\n");
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }
    return 0;
}
