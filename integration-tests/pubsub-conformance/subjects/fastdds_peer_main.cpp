// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The Fast DDS peer child: a provider factory and nothing else. The request /
// reply loop and the protocol live in src/peer_main.cpp, once.

#include <cstdint>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <memory>
#include <string>

#include "fletcher/conformance/peer.hpp"

int main(int argc, char** argv) {
    return fletcher::conformance::RunPeerMain(
        argc, argv, [](int count, char** args) -> std::shared_ptr<fletcher::PubSubProvider> {
            fletcher::FastDDSProviderOptions options;
            for (int i = 1; i < count; ++i) {
                if (std::string(args[i]) == "--domain-id" && i + 1 < count) {
                    options.domain_id = static_cast<uint32_t>(std::stoul(args[++i]));
                }
            }
            return std::make_shared<fletcher::FastDDSPubSubProvider>(std::move(options));
        });
}
