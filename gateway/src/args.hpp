// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_GATEWAY_SRC_ARGS_HPP_
#define FLETCHER_GATEWAY_SRC_ARGS_HPP_

#include <cstdint>
#include <optional>
#include <string>

namespace gateway {

/// false = Fletcher default reader profile (RELIABLE + KEEP_ALL + TRANSIENT_LOCAL); true = VOLATILE
/// + KEEP_LAST(depth), so late joiners skip writer history and reader memory stays bounded on
/// high-rate streams.
struct ReaderQosSpec {
    bool use_volatile = false;
    int32_t depth = 32;
};

/// Parses "default" | "volatile" | "volatile:<depth>" (depth >= 1).
std::optional<ReaderQosSpec> ParseReaderQos(const std::string& value);

struct Args {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 9090;
    std::string provider = "inprocess";
    uint32_t domain_id = 0;
    /// 0 = provider default; the bound is baked into the DDS type name (fletcher_<bytes>) so it
    /// must match the bus exactly.
    uint32_t max_payload_bytes = 0;
    ReaderQosSpec reader_qos{};
    bool show_version = false;
    bool show_help = false;
};

/// Pure parse (no IO, no exit): nullopt + `error` on invalid input; --help/--version set their flag
/// and stop parsing.
std::optional<Args> ParseArgs(int argc, const char* const argv[], std::string& error);

const char* UsageString();

}  // namespace gateway

#endif  // FLETCHER_GATEWAY_SRC_ARGS_HPP_
