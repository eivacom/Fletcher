// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The Fast DDS peer child: a provider factory and nothing else. The request /
// reply loop and the protocol live in src/peer_main.cpp, once.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fletcher/conformance/peer.hpp"

namespace {

// Writer-side half of the readiness fence (peer.hpp `await_matched`): records
// on_publication_matched for the DATA (non-schema) writer. Same threading
// contract as the harness's reader-side MatchTracker (subjects/fastdds_main.cpp):
// no provider call, no blocking, no throw inside OnMatched. Static so it
// outlives the provider, as fast_dds_pubsub_provider.hpp requires.
class WriterMatchTracker : public fletcher::FastDDSStatusListener {
   public:
    void OnMatched(Endpoint endpoint, int32_t current_count, int32_t /*change*/) noexcept override {
        if (endpoint.is_schema_channel || !endpoint.is_writer) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            counts_[std::string(endpoint.topic)] = current_count;
        }
        cv_.notify_all();
    }

    void AwaitMatched(const std::string& topic_name, std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, budget, [&] {
            auto it = counts_.find(topic_name);
            return it != counts_.end() && it->second >= 1;
        });
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, int32_t> counts_;
};

WriterMatchTracker g_writer_matches;

}  // namespace

int main(int argc, char** argv) {
    return fletcher::conformance::RunPeerMain(
        argc, argv,
        [](int count, char** args) -> std::shared_ptr<fletcher::PubSubProvider> {
            fletcher::ProviderConfig config;
            for (int i = 1; i < count; ++i) {
                if (std::string(args[i]) == "--domain-id" && i + 1 < count) {
                    config.domain_id = static_cast<uint32_t>(std::stoul(args[++i]));
                }
            }
            return std::make_shared<fletcher::FastDDSPubSubProvider>(config, &g_writer_matches);
        },
        [](const std::vector<std::string>& topic, std::chrono::milliseconds budget) {
            g_writer_matches.AwaitMatched(fletcher::internal::JoinSegments(topic), budget);
        });
}
