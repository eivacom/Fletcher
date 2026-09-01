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
//   --max-payload-bytes N  DDS payload bound for the fastdds provider
//                          (default 1 MiB). Part of the DDS type name
//                          (fletcher_<bytes>), so it must match every
//                          peer on the bus exactly.
//   --reader-qos P         fastdds reader QoS profile: "default"
//                          (Fletcher profile: RELIABLE + KEEP_ALL +
//                          TRANSIENT_LOCAL) or "volatile[:depth]"
//                          (VOLATILE + KEEP_LAST(depth), default 32)
//                          for high-rate streams where late joiners
//                          must not replay history.
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

#include "args.hpp"
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
            slot.schema = fletcher::MakeSharedSchema(fletcher::OwnedSchema::DeepCopy(schema.get()));
        }
    }

    // mu_ is held across the callback: one delivery at a time, so a callback must not re-enter.
    void Publish(const std::vector<std::string>& segments, const RowEncoder& encoder,
                 const fletcher::Attachments& attachments) override {
        fletcher::VectorWriteBuffer wb;
        encoder(wb);
        const std::vector<uint8_t> buf = wb.Finish();

        std::lock_guard lock(mu_);
        auto [it, _] = topics_.try_emplace(Join(segments));
        if (it->second.callback) {
            it->second.callback(buf.data(), buf.size(), it->second.schema, attachments);
        }
    }

    fletcher::SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                           SubscribeCallback callback) override {
        std::lock_guard lock(mu_);
        auto& slot = topics_[Join(segments)];
        slot.callback = std::move(callback);
        return {fletcher::MakeReadySchemaFuture(slot.schema)};
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
        // Null when nobody announced one; the gateway lets the client bring its own.
        fletcher::SharedSchema schema;
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

}  // namespace

int main(int argc, char* argv[]) {
    std::string parse_error;
    auto parsed = gateway::ParseArgs(argc, argv, parse_error);
    if (!parsed) {
        std::fprintf(stderr, "fletcher-gateway: %s\n", parse_error.c_str());
        return 2;
    }
    const gateway::Args& args = *parsed;
    if (args.show_version) {
        std::printf("fletcher-gateway %s\n", GATEWAY_VERSION_STRING);
        return 0;
    }
    if (args.show_help) {
        std::printf("Usage: %s %s\n", argv[0], gateway::UsageString());
        return 0;
    }

    std::shared_ptr<fletcher::PubSubProvider> provider;
    if (args.provider == "fastdds") {
        fletcher::FastDDSProviderOptions dds_opts;
        dds_opts.domain_id = args.domain_id;
        if (args.max_payload_bytes != 0) {
            dds_opts.max_payload_bytes = args.max_payload_bytes;
        }
        if (args.reader_qos.use_volatile) {
            auto qos = fletcher::internal::MakeFletcherDefaultReaderQos();
            qos.durability().kind = eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS;
            qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
            qos.history().depth = args.reader_qos.depth;
            dds_opts.default_reader_qos = qos;
        }
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
