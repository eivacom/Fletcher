// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The XRCE-DDS peer child. Same shape as the Fast DDS peer: a factory only.
//
// Its session key must differ from every other client on one Agent, including
// every EARLIER child of this same subject — a key reused across sequential
// children makes each create_session race the previous session's teardown. The
// parent passes a base and this process adds its own pid, which is unique per
// child by construction and needs no coordination.

#include <cstdint>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <memory>
#include <string>

#include "fletcher/conformance/peer.hpp"

#ifdef _WIN32
#include <process.h>
#define FLETCHER_GETPID _getpid
#else
#include <unistd.h>
#define FLETCHER_GETPID getpid
#endif

int main(int argc, char** argv) {
    return fletcher::conformance::RunPeerMain(
        argc, argv, [](int count, char** args) -> std::shared_ptr<fletcher::PubSubProvider> {
            fletcher::XrceConfig config;
            config.connect_timeout_ms = 5000;
            for (int i = 1; i < count; ++i) {
                const std::string arg = args[i];
                if (arg == "--domain-id" && i + 1 < count) {
                    config.domain_id = static_cast<uint16_t>(std::stoul(args[++i]));
                } else if (arg == "--agent-port" && i + 1 < count) {
                    config.agent_port = static_cast<uint16_t>(std::stoul(args[++i]));
                } else if (arg == "--session-key-base" && i + 1 < count) {
                    const uint32_t base = static_cast<uint32_t>(std::stoul(args[++i], nullptr, 0));
                    config.session_key =
                        base + (static_cast<uint32_t>(FLETCHER_GETPID()) & 0x0FFFu);
                }
            }
            return std::make_shared<fletcher::XrceDDSPubSubProvider>(config);
        });
}
