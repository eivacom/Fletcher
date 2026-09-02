// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// See internal/xrce_document.hpp for the format, the tolerance rules and why this reader is
// deliberately not shared with the loopback's.

#include "internal/xrce_document.hpp"

#include <fletcher/core/status.hpp>
#include <limits>
#include <string>

namespace fletcher {
namespace internal {
namespace {

// Quote one document entry for a refusal message. Plain concatenation, not escaping: a document
// containing a NUL is refused before this can be called, so there is no byte here that could
// hide inside the quotes or truncate the message on its way out through `PubSubError::what()`.
// Deliberately not the shared `Quoted` helper in provider_registry.cpp, nor the loopback's
// (decision 8: no shared parser, and no dependency between a provider TU and the registry TU).
std::string QuoteEntry(const std::string& entry) { return "\"" + entry + "\""; }

// Strict decimal, parsed WIDE. `false` for an empty string, for any byte that is not `0`-`9`
// (so no sign, no `0x`, no whitespace, no separators - one total rule beats two, and every
// caller in the tree builds its keys with `std::to_string`) and for anything past 2^64-1.
//
// The caller range-checks per key against the wide value, so **no value is ever narrowed
// silently** (rung-1 case 3): a domain-sized truncation is a wrong answer with no error, which
// is the one outcome this reader may not produce.
bool ParseDecimal(const std::string& text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

[[noreturn]] void Refuse(const std::string& reason, const std::string& entry) {
    throw PubSubError(PubSubStatus::kInvalidArgument, "XRCE: " + reason + ": " + QuoteEntry(entry));
}

}  // namespace

XrceSettings ParseXrceDocument(const ProviderConfig& config) {
    const std::string& document = config.document;

    const size_t nul = document.find('\0');
    if (nul != std::string::npos) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "XRCE: document contains a NUL at offset " + std::to_string(nul) +
                              "; this provider's key=value format has no representation for one");
    }

    XrceSettings settings;
    bool seen_transport = false;
    bool seen_agent = false;
    bool seen_session_key = false;
    bool seen_connect_timeout = false;

    size_t start = 0;
    while (start <= document.size()) {
        const size_t nl = document.find('\n', start);
        const size_t end = (nl == std::string::npos) ? document.size() : nl;
        std::string entry = document.substr(start, end - start);
        if (!entry.empty() && entry.back() == '\r') {
            entry.pop_back();
        }
        start = (nl == std::string::npos) ? document.size() + 1 : nl + 1;

        if (entry.empty()) continue;

        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            Refuse("document entry with no '='", entry);
        }
        const std::string key = entry.substr(0, eq);
        const std::string value = entry.substr(eq + 1);

        if (key == "transport") {
            if (seen_transport) Refuse("duplicate document key", entry);
            seen_transport = true;
            if (value == "udp") {
                settings.transport = XrceTransportKind::kUdp;
            } else if (value == "tcp") {
                settings.transport = XrceTransportKind::kTcp;
            } else if (value == "serial") {
                // Nameable, and refused DISTINCTLY from a typo: an operator who asked for
                // serial has to change their hardware plan, an operator who mistyped a key has
                // to change one character (owner ruling 2026-09-02, "accept it, fail
                // distinctly"). Refused HERE, so no transport object ever exists for it.
                throw PubSubError(PubSubStatus::kNotSupported,
                                  "XRCE: this build cannot do serial transport; only 'udp' and "
                                  "'tcp' are implemented: " +
                                      QuoteEntry(entry));
            } else {
                Refuse("unknown transport, expected 'udp', 'tcp' or 'serial'", entry);
            }
        } else if (key == "agent") {
            if (seen_agent) Refuse("duplicate document key", entry);
            seen_agent = true;
            // ONE key, not two. Separate `agent_ip` / `agent_port` let a document name only the
            // host and silently keep port 2018 - a half-specified address, the "silence is
            // load-bearing" trap PDA-DEC-6 paid a cycle for. One line cannot be half-given, and
            // the ruling's "becomes a document line" is taken literally.
            const size_t colon = value.find(':');
            if (colon == std::string::npos || value.find(':', colon + 1) != std::string::npos) {
                // Also the rule that forecloses IPv6: `[::1]:2018` and `::1` both land here.
                // The client is initialised UXR_IPv4, so nothing is lost that ever worked -
                // disclosed in the README rather than half-accepted.
                Refuse(
                    "agent must be HOST:PORT with exactly one colon (an IPv4 literal or a "
                    "hostname; IPv6 is not supported)",
                    entry);
            }
            const std::string host = value.substr(0, colon);
            if (host.empty()) {
                Refuse("agent has an empty host", entry);
            }
            uint64_t port = 0;
            if (!ParseDecimal(value.substr(colon + 1), port) || port == 0u || port > 65535u) {
                Refuse("agent port must be a decimal number between 1 and 65535", entry);
            }
            settings.agent_host = host;
            settings.agent_port = static_cast<uint16_t>(port);
        } else if (key == "session_key") {
            if (seen_session_key) Refuse("duplicate document key", entry);
            seen_session_key = true;
            uint64_t session_key = 0;
            if (!ParseDecimal(value, session_key) ||
                session_key > std::numeric_limits<uint32_t>::max()) {
                Refuse("session_key must be a decimal number between 0 and 4294967295", entry);
            }
            settings.session_key = static_cast<uint32_t>(session_key);
        } else if (key == "connect_timeout_ms") {
            if (seen_connect_timeout) Refuse("duplicate document key", entry);
            seen_connect_timeout = true;
            uint64_t timeout_ms = 0;
            if (!ParseDecimal(value, timeout_ms) || timeout_ms > 60000u) {
                Refuse("connect_timeout_ms must be a decimal number between 0 and 60000", entry);
            }
            settings.connect_timeout = std::chrono::milliseconds(static_cast<int64_t>(timeout_ms));
        } else {
            // `stream_history` and `run_loop_ms` were typed fields before this item and are
            // gone: no in-tree caller set either and nothing could observe either, so a key for
            // one would have been a range-check with nothing behind it. They arrive here as
            // typos like any other name, which is the point (design §2, README).
            Refuse(
                "unknown document key, expected one of 'transport', 'agent', 'session_key', "
                "'connect_timeout_ms'",
                entry);
        }
    }

    return settings;
}

}  // namespace internal
}  // namespace fletcher
