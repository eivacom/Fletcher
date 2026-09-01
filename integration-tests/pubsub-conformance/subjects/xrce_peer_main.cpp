// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The XRCE-DDS peer child. Same shape as the Fast DDS peer: a factory only.
// Its session key must differ from every other client on the Agent, so the
// parent passes one in.

#include <cstdint>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <memory>
#include <string>

#include "fletcher/conformance/peer.hpp"

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
                } else if (arg == "--session-key" && i + 1 < count) {
                    config.session_key = static_cast<uint32_t>(std::stoul(args[++i], nullptr, 0));
                }
            }
            return std::make_shared<fletcher::XrceDDSPubSubProvider>(config);
        });
}
