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

// Named WriteReply, not Reply: conformance::Reply is the subject-side reply type.
void WriteReply(const std::string& line) {
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

    WriteReply("READY");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream in(line);
        // Every reply echoes the request's tag; see peer.hpp. A request with no
        // verb is ignored rather than answered, so a blank line cannot consume
        // a tag the parent is waiting on.
        std::string tag;
        std::string verb;
        in >> tag >> verb;
        if (tag.empty() || verb.empty()) {
            continue;
        }
        const std::string prefix = tag + " ";
        if (verb == "quit") {
            WriteReply(prefix + "ok");
            break;
        }
        try {
            if (verb == "create") {
                std::string joined;
                std::string which;
                in >> joined >> which;
                provider->CreateTopic(SplitTopic(joined),
                                      MakeConformanceSchema(ParseSchemaId(which)));
                WriteReply(prefix + "ok");
            } else if (verb == "publish") {
                std::string joined;
                uint32_t seq = 0;
                in >> joined >> seq;
                provider->Publish(SplitTopic(joined),
                                  [seq](WriteBuffer& buf) { EncodeRow(buf, seq); });
                WriteReply(prefix + "ok");
            } else {
                WriteReply(prefix + "err peer: unknown verb: " + verb);
            }
        } catch (const std::exception& e) {
            WriteReply(prefix + "err " + DescribeException(e));
        } catch (...) {
            WriteReply(prefix + "err unknown exception: " + verb);
        }
    }
    return 0;
}

}  // namespace conformance
}  // namespace fletcher
