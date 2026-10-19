// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C++ peer of the cross-language lane (BIND-8d, D-BIND-116): the generated C++ native pair
// over Fast DDS, on the domain of the gateway the C# gateway client talks to.
//
//   * CsToCpp: subscribes at start; compares every row it receives, field by field, with the
//     fixture row its `seq` names, and prints `RECV <seq> OK` or `RECV <seq> MISMATCH <diffs>`.
//   * CppToCs: declares the topic at start; publishes the four fixture rows on the stdin line
//     `publish` and then prints `PUBLISHED 4`; on `probe` it publishes one row no receiver
//     compares (seq -1) and prints `PROBED`, so the C# side can wait for a live match (D-BIND-129).
//
// Prints READY once both endpoints exist, exits on the stdin line `stop`. The fixture rows are
// protoc-coverage's (tests/coverage_fixture.hpp), the ones its goldens pin.
//
// CLI: --domain-id N --document <path of the Fast DDS profile document the gateway loads too>

#include <cstdint>
#include <cstdio>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fstream>
#include <iostream>
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

    auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>(
        fletcher::ProviderConfig{.domain_id = domain_id, .document = document});

    xl::CrossLang_CsToCppSubscriber from_cs(provider);
    from_cs.Subscribe([](xl::CrossRow row, const fletcher::Attachments&) {
        PrintLine(crosslang::ReportRow(row.seq(), row.row()));
    });

    xl::CrossLang_CppToCsPublisher to_cs(provider);

    PrintLine("READY");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") break;
        if (line == "probe") {
            // D-BIND-129: one row no receiver compares (seq -1); the C# side sends this until
            // one arrives, so the fixture rows that follow travel live.
            xl::CrossRow probe;
            probe.set_seq(-1);
            to_cs.Publish(probe);
            PrintLine("PROBED");
        }
        if (line == "publish") {
            for (int32_t seq = 0; seq < crosslang::kRowCount; ++seq) {
                xl::CrossRow row;
                row.set_seq(seq).set_row(crosslang::FixtureRow(seq));
                to_cs.Publish(row);
            }
            PrintLine("PUBLISHED " + std::to_string(crosslang::kRowCount));
        }
    }
    return 0;
}
