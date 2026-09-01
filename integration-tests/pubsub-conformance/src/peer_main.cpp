// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <cstdio>
#include <exception>
#include <fletcher/core/write_buffer.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "fletcher/conformance/fixtures.hpp"
#include "fletcher/conformance/peer.hpp"
#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {
namespace {

Topic SplitTopic(const std::string& joined) {
    Topic segments;
    std::string current;
    for (char c : joined) {
        if (c == '/') {
            segments.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    segments.push_back(current);
    return segments;
}

void Reply(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

SchemaId ParseSchemaId(const std::string& token) {
    if (token == "A") {
        return SchemaId::kA;
    }
    if (token == "B") {
        return SchemaId::kB;
    }
    return SchemaId::kNone;
}

}  // namespace

int RunPeerMain(int argc, char** argv, const PeerProviderFactory& make_provider) {
    std::shared_ptr<PubSubProvider> provider;
    try {
        provider = make_provider(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "conformance peer: cannot create provider: %s\n", e.what());
        return 1;
    }
    if (!provider) {
        std::fprintf(stderr, "conformance peer: provider factory returned null\n");
        return 1;
    }

    Reply("READY");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream in(line);
        std::string verb;
        in >> verb;
        if (verb.empty()) {
            continue;
        }
        if (verb == "quit") {
            Reply("ok");
            break;
        }
        try {
            if (verb == "create") {
                std::string joined;
                std::string which;
                in >> joined >> which;
                provider->CreateTopic(SplitTopic(joined),
                                      MakeConformanceSchema(ParseSchemaId(which)));
                Reply("ok");
            } else if (verb == "publish") {
                std::string joined;
                uint32_t seq = 0;
                in >> joined >> seq;
                provider->Publish(SplitTopic(joined),
                                  [seq](WriteBuffer& buf) { EncodeRow(buf, seq); });
                Reply("ok");
            } else {
                Reply("err peer: unknown verb: " + verb);
            }
        } catch (const std::exception& e) {
            Reply("err " + DescribeException(e));
        } catch (...) {
            Reply("err unknown exception: " + verb);
        }
    }
    return 0;
}

}  // namespace conformance
}  // namespace fletcher
