// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// fletcher-gateway — Fletcher's WebSocket gateway server.
//
// Distributed as a single executable. The only supported integration
// point is the WebSocket protocol — there is no public C++ API.
//
//   gateway --port 9090 --bind-address 0.0.0.0
//
// CLI:
//   --port N               TCP port to listen on (default 9090).
//   --bind-address ADDR    bind address (default 0.0.0.0).
//   --provider TYPE        pub/sub provider: "inprocess" (default) or
//                          "fastdds". Both are compiled in; the switch
//                          selects between them at runtime.
//   --domain-id N          DDS domain id for the fastdds provider (default 0).
//                          Ignored by the inprocess provider.
//
// The gateway is schema-agnostic. It knows nothing about topic schemas
// or which topics exist before clients show up; clients establish
// topics by subscribing or publishing and supply their own schemas
// (typically generated from a `.proto` by protoc-gen-fletcher).
//
// Process lifecycle:
//   * Prints "READY <port>" on stdout once the gateway is accepting
//     connections so launchers can synchronise without polling the
//     socket.
//   * Reads stdin and exits cleanly on the literal line "stop".
//     Deterministic shutdown without relying on SIGTERM ordering
//     (Windows SIGTERM semantics differ from POSIX).
//
// Provider note:
//   The default provider is an in-process loopback (--provider inprocess),
//   which only connects WebSocket clients on the same process. The
//   DDS-backed provider (--provider fastdds) bridges the gateway to any
//   FastDDS app on the same DDS domain, so a WebSocket client can pub/sub
//   to data flowing over DDS. Both providers are always compiled into the
//   exe; the switch selects between them at runtime.

#include <cstdio>
#include <cstdlib>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "gateway.hpp"

namespace {

// ─────────────────────────────────────────────────────────────────────
// InProcessProvider — the only provider implementation gateway ships
// with right now. Topics are created on first subscribe or publish so
// no pre-registration is needed.
//
// The provider caches the schema a publisher announced via
// CreateTopic and hands it back to subscribers via SubscriptionResult.
// Gateway forwards that schema to its WebSocket clients on subscribe,
// but never inspects or validates it — pure passthrough.
// ─────────────────────────────────────────────────────────────────────
class InProcessProvider : public fletcher::PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema schema) override {
        std::lock_guard lock(mu_);
        auto& slot = topics_[Join(segments)];
        if (schema) {
            slot.schema = fletcher::OwnedSchema::DeepCopy(schema.get());
        }
    }

    void Publish(const std::vector<std::string>& segments, RowEncoder encoder,
                 const fletcher::Attachments& attachments) override {
        std::vector<uint8_t> buf;
        fletcher::VectorWriteBuffer wb(buf);
        encoder(wb);

        SubscribeCallback cb;
        {
            std::lock_guard lock(mu_);
            auto [it, _] = topics_.try_emplace(Join(segments));
            cb = it->second.callback;
        }
        if (cb) {
            cb(buf.data(), buf.size(), nullptr, attachments);
        }
    }

    fletcher::SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                           SubscribeCallback callback) override {
        std::lock_guard lock(mu_);
        auto& slot = topics_[Join(segments)];
        slot.callback = std::move(callback);
        fletcher::SharedSchema schema;
        if (slot.schema) {
            schema = fletcher::MakeSharedSchema(fletcher::OwnedSchema::DeepCopy(slot.schema.get()));
        }
        return {fletcher::MakeReadySchemaFuture(std::move(schema))};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        std::lock_guard lock(mu_);
        auto it = topics_.find(Join(segments));
        if (it != topics_.end()) {
            it->second.callback = nullptr;
        }
    }

   private:
    struct TopicState {
        SubscribeCallback callback;
        fletcher::OwnedSchema schema;
    };
    std::mutex mu_;
    std::unordered_map<std::string, TopicState> topics_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) {
                out += '/';
            }
            out += segs[i];
        }
        return out;
    }
};

struct Args {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 9090;
    std::string provider = "inprocess";
    uint32_t domain_id = 0;
};

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bind-address" && i + 1 < argc) {
            a.bind_address = argv[++i];
        } else if (arg == "--provider" && i + 1 < argc) {
            a.provider = argv[++i];
        } else if (arg == "--domain-id" && i + 1 < argc) {
            a.domain_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--version") {
            std::printf("fletcher-gateway %s\n", GATEWAY_VERSION_STRING);
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: %s [--port N] [--bind-address ADDR] "
                "[--provider inprocess|fastdds] [--domain-id N] [--version]\n",
                argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "fletcher-gateway: unknown argument: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    if (a.provider != "inprocess" && a.provider != "fastdds") {
        std::fprintf(stderr,
                     "fletcher-gateway: unknown provider: %s (expected inprocess|fastdds)\n",
                     a.provider.c_str());
        std::exit(2);
    }
    return a;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fletcher-gateway: bad CLI: %s\n", e.what());
        return 2;
    }

    std::shared_ptr<fletcher::PubSubProvider> provider;
    if (args.provider == "fastdds") {
        fletcher::FastDDSProviderOptions dds_opts;
        dds_opts.domain_id = args.domain_id;
        provider = std::make_shared<fletcher::FastDDSPubSubProvider>(std::move(dds_opts));
    } else {
        provider = std::make_shared<InProcessProvider>();
    }

    auto publisher = std::make_shared<fletcher::Publisher>(provider);
    auto subscriber = std::make_shared<fletcher::Subscriber>(provider);

    fletcher::GatewayOptions opts;
    opts.address = args.bind_address;
    opts.port = args.port;

    fletcher::Gateway gw(std::move(publisher), std::move(subscriber), opts);
    gw.Start();

    std::printf("READY %u\n", args.port);
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }

    gw.Stop();
    return 0;
}
