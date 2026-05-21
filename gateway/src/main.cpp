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
//   The exe currently always uses an in-process loopback provider —
//   the only provider implementation gateway has. A real DDS-backed
//   provider is tracked separately; once it exists this same exe
//   will gain a `--provider TYPE` switch.

#include "gateway.hpp"

#include <fletcher/pubsub/driver.hpp>
#include <fletcher/pubsub/pubsub.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/core/write_buffer.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

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
class InProcessProvider : public fletcher::PubSub {
 public:
    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema schema,
                     std::any /*config*/) override {
        std::lock_guard lock(mu_);
        auto& slot = topics_[Join(segments)];
        if (schema) {
            slot.schema = fletcher::OwnedSchema::DeepCopy(schema.get());
        }
    }

    void Publish(const std::vector<std::string>& segments,
                 RowEncoder encoder,
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

    fletcher::SubscriptionResult Subscribe(
        const std::vector<std::string>& segments,
        SubscribeCallback callback,
        std::any /*config*/) override {
        std::lock_guard lock(mu_);
        auto& slot = topics_[Join(segments)];
        slot.callback = std::move(callback);
        fletcher::SubscriptionResult result;
        if (slot.schema) {
            result.schema = fletcher::OwnedSchema::DeepCopy(slot.schema.get());
        }
        return result;
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
    uint16_t    port = 9090;
};

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bind-address" && i + 1 < argc) {
            a.bind_address = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: %s [--port N] [--bind-address ADDR]\n",
                argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "fletcher-gateway: unknown argument: %s\n",
                         arg.c_str());
            std::exit(2);
        }
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

    auto provider = std::make_shared<InProcessProvider>();
    auto driver = std::make_shared<fletcher::Driver>(provider);

    fletcher::GatewayOptions opts;
    opts.address = args.bind_address;
    opts.port    = args.port;

    fletcher::Gateway gw(driver, opts);
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
