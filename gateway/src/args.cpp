// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "args.hpp"

#include <charconv>

namespace gateway {

namespace {

template <typename T>
bool ParseNumber(const std::string& s, T& out) {
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

}  // namespace

std::optional<ReaderQosSpec> ParseReaderQos(const std::string& value) {
    if (value == "default") {
        return ReaderQosSpec{};
    }
    ReaderQosSpec spec;
    spec.use_volatile = true;
    if (value == "volatile") {
        return spec;
    }
    constexpr const char kPrefix[] = "volatile:";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (value.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    if (!ParseNumber(value.substr(kPrefixLen), spec.depth) || spec.depth < 1) {
        return std::nullopt;
    }
    return spec;
}

const char* UsageString() {
    return "[--port N] [--bind-address ADDR] [--provider inprocess|fastdds] [--domain-id N] "
           "[--max-payload-bytes N] [--reader-qos default|volatile[:depth]] [--version]";
}

std::optional<Args> ParseArgs(int argc, const char* const argv[], std::string& error) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            uint32_t port = 0;
            if (!ParseNumber(argv[++i], port) || port < 1 || port > 65535) {
                error = "bad --port: " + std::string(argv[i]);
                return std::nullopt;
            }
            a.port = static_cast<uint16_t>(port);
        } else if (arg == "--bind-address" && i + 1 < argc) {
            a.bind_address = argv[++i];
        } else if (arg == "--provider" && i + 1 < argc) {
            a.provider = argv[++i];
        } else if (arg == "--domain-id" && i + 1 < argc) {
            if (!ParseNumber(argv[++i], a.domain_id)) {
                error = "bad --domain-id: " + std::string(argv[i]);
                return std::nullopt;
            }
        } else if (arg == "--max-payload-bytes" && i + 1 < argc) {
            if (!ParseNumber(argv[++i], a.max_payload_bytes) || a.max_payload_bytes == 0) {
                error = "bad --max-payload-bytes: " + std::string(argv[i]);
                return std::nullopt;
            }
        } else if (arg == "--reader-qos" && i + 1 < argc) {
            auto spec = ParseReaderQos(argv[++i]);
            if (!spec) {
                error = "bad --reader-qos: " + std::string(argv[i]) +
                        " (expected default|volatile[:depth])";
                return std::nullopt;
            }
            a.reader_qos = *spec;
        } else if (arg == "--version") {
            a.show_version = true;
            return a;
        } else if (arg == "--help" || arg == "-h") {
            a.show_help = true;
            return a;
        } else {
            error = "unknown argument: " + arg;
            return std::nullopt;
        }
    }
    if (a.provider != "inprocess" && a.provider != "fastdds") {
        error = "unknown provider: " + a.provider + " (expected inprocess|fastdds)";
        return std::nullopt;
    }
    return a;
}

}  // namespace gateway
