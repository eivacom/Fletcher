// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C++ peer of the cross-language lane (BIND-8d, D-BIND-116; BIND-9b-4, D-BIND-161): the
// generated C++ native pair over Fast DDS, on the domain of the gateway the C# gateway client
// talks to.
//
//   * CsToCpp, NativeToCpp: subscribes at start; compares every row it receives, field by field,
//     with the fixture row its `seq` names, and prints `<topic> RECV <seq> OK` or
//     `<topic> RECV <seq> MISMATCH <diffs>`.
//   * CppToCs, CppToNative: declares the topics at start; on the stdin line `publish <topic>`
//     publishes the four fixture rows on it and prints `PUBLISHED 4`; on `probe <topic>` it
//     publishes one row no receiver compares (seq -1) and prints `PROBED`, so the receiving side
//     can wait for a live match (D-BIND-129). Any other topic prints `ERROR`.
//
// Every line names its topic because a peer listens on more than one: a late row of an earlier
// test must not count for the next one.
//
// Prints READY once every endpoint exists, exits on the stdin line `stop`. The fixture rows are
// protoc-coverage's (tests/coverage_fixture.hpp), the ones its goldens pin.
//
// CLI: --domain-id N --document <path of the Fast DDS profile document the gateway loads too>

#include <cstdint>
#include <cstdio>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include "coverage_fixture.hpp"
#include "crosslang.fletcher.pb.h"
#include "row_compare.hpp"

namespace {

namespace xl = fletcher_gen::gateway::crosslang;

std::mutex g_out;

void PrintLine(const std::string& line) {
    std::lock_guard lock(g_out);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Prints the transport's statuses as `STATUS <topic> <schema|data> <reader|writer> ...` lines,
// which the test ignores until a row fails to arrive, and then shows: whether the schema channel
// matched, whether a data reader followed, and whether samples were lost (BIND-9b-6: NativeToCpp
// once delivered nothing on the CI runner, not even a probe, and never locally).
class StatusPrinter : public fletcher::FastDDSStatusListener {
   public:
    void OnMatched(Endpoint endpoint, int32_t current_count, int32_t change) noexcept override {
        Print(endpoint,
              "matched " + std::to_string(current_count) + " (" + std::to_string(change) + ")");
    }
    void OnIncompatibleQos(Endpoint endpoint, uint32_t policy_id,
                           uint32_t total_count) noexcept override {
        Print(endpoint, "incompatible-qos policy " + std::to_string(policy_id) + " total " +
                            std::to_string(total_count));
    }
    void OnSampleLost(Endpoint endpoint, uint32_t total_count) noexcept override {
        Print(endpoint, "sample-lost total " + std::to_string(total_count));
    }
    void OnSampleRejected(Endpoint endpoint, int32_t reason,
                          uint32_t total_count) noexcept override {
        Print(endpoint, "sample-rejected reason " + std::to_string(reason) + " total " +
                            std::to_string(total_count));
    }

   private:
    static void Print(Endpoint endpoint, const std::string& what) noexcept {
        try {
            PrintLine("STATUS " + std::string(endpoint.topic) +
                      (endpoint.is_schema_channel ? " schema " : " data ") +
                      (endpoint.is_writer ? "writer " : "reader ") + what);
        } catch (...) {
        }
    }
};

// Reports every row a subscriber receives, under the topic's name.
auto Reporter(std::string topic) {
    return [topic = std::move(topic)](xl::CrossRow row, const fletcher::Attachments&) {
        PrintLine(topic + " " + crosslang::ReportRow(row.seq(), row.row()));
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    uint32_t domain_id = 0;
    std::string document;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--domain-id")
            domain_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (arg == "--document")
            document = ReadFile(argv[++i]);
    }

    // Outlives the provider, as the listener contract requires.
    static StatusPrinter status_printer;
    auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>(
        fletcher::ProviderConfig{.domain_id = domain_id, .document = document}, &status_printer);

    xl::CrossLang_CsToCppSubscriber from_cs(provider);
    from_cs.Subscribe(Reporter("CsToCpp"));
    xl::CrossLang_NativeToCppSubscriber from_native(provider);
    from_native.Subscribe(Reporter("NativeToCpp"));

    xl::CrossLang_CppToCsPublisher to_cs(provider);
    xl::CrossLang_CppToNativePublisher to_native(provider);
    const std::map<std::string, std::function<void(const xl::CrossRow&)>> publishers{
        {"CppToCs", [&](const xl::CrossRow& row) { to_cs.Publish(row); }},
        {"CppToNative", [&](const xl::CrossRow& row) { to_native.Publish(row); }},
    };

    PrintLine("READY");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") break;
        const auto space = line.find(' ');
        const std::string command = line.substr(0, space);
        const std::string topic = space == std::string::npos ? "" : line.substr(space + 1);
        if (command != "probe" && command != "publish") continue;
        const auto publisher = publishers.find(topic);
        if (publisher == publishers.end()) {
            PrintLine("ERROR no publisher for topic '" + topic + "'");
            continue;
        }
        if (command == "probe") {
            // D-BIND-129: one row no receiver compares (seq -1); the receiving side has this
            // sent until one arrives, so the fixture rows that follow travel live.
            xl::CrossRow probe;
            probe.set_seq(-1);
            publisher->second(probe);
            PrintLine("PROBED");
        } else {
            for (int32_t seq = 0; seq < crosslang::kRowCount; ++seq) {
                xl::CrossRow row;
                row.set_seq(seq).set_row(crosslang::FixtureRow(seq));
                publisher->second(row);
            }
            PrintLine("PUBLISHED " + std::to_string(crosslang::kRowCount));
        }
    }
    return 0;
}
