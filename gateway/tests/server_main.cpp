// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// gateway-test-server — a runnable exe that wraps WebGateway with an
// InProcessProvider, used for end-to-end and ad-hoc testing of the
// Fletcher WebSocket protocol. Not a production binary: gateway does
// not have a real DDS-backed provider yet; once one exists this fixture
// should be replaced by a real `gateway` binary that accepts a
// provider configuration.
//
// CLI:
//   --port N              TCP port to bind on 127.0.0.1 (default 9091)
//   --heartbeat-ms N      milliseconds between heartbeat publishes
//                         (default 100; set to 0 to disable)
//   --bind-address ADDR   bind address (default 127.0.0.1; tests run
//                         on loopback, but accepting an override keeps
//                         the door open for container runs)
//
// Runtime behaviour:
//   * Pre-creates two topics with a three-field schema
//     {sensor_id : int32, temperature : float64, label : utf8}:
//       - "heartbeat" — server publishes a synthetic row every
//         --heartbeat-ms; lets a subscribed client verify the
//         publish-from-server path.
//       - "echo" — InProcessProvider routes Publish() back to the
//         registered subscriber callback, so a client that subscribes
//         and then publishes proves the client-publish path without a
//         side channel.
//   * Prints "READY <port>" on stdout once the gateway is accepting
//     connections; test runners synchronise on that line rather than
//     polling the port.
//   * Reads stdin and exits cleanly on the literal line "stop". Gives
//     test runners deterministic shutdown across platforms (Windows
//     SIGTERM semantics differ from POSIX).

#include <web_gateway/web_gateway.hpp>
#include <pubsub/driver.hpp>
#include <pubsub/pubsub.hpp>
#include <pubsub/owned_schema.hpp>
#include <core/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// ─────────────────────────────────────────────────────────────────────
// InProcessProvider — single callback per topic, Publish() invokes
// it synchronously. Mirrors the contract documented in
// pubsub/include/pubsub/pubsub.hpp.
// ─────────────────────────────────────────────────────────────────────
class InProcessProvider : public fletcher::PubSub {
 public:
    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema /*schema*/,
                     std::any /*config*/) override {
        std::lock_guard lock(mu_);
        topics_[Join(segments)] = {};
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
            auto it = topics_.find(Join(segments));
            if (it == topics_.end()) {
                return;
            }
            cb = it->second;
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
        topics_[Join(segments)] = std::move(callback);
        return {};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        std::lock_guard lock(mu_);
        auto it = topics_.find(Join(segments));
        if (it != topics_.end()) {
            it->second = nullptr;
        }
    }

 private:
    std::mutex mu_;
    std::unordered_map<std::string, SubscribeCallback> topics_;

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

fletcher::OwnedSchema BuildSensorSchema() {
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 3);
    ArrowSchemaSetName(schema->children[0], "sensor_id");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    ArrowSchemaSetName(schema->children[1], "temperature");
    ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_DOUBLE);
    ArrowSchemaSetName(schema->children[2], "label");
    ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_STRING);
    return schema;
}

void EncodeSensorRow(fletcher::WriteBuffer& buf,
                     int32_t sensor_id,
                     double temperature,
                     const std::string& label) {
    buf.AppendByte(0x00);  // null bitfield: 3 fields, none null
    buf.AppendFixed<int32_t>(sensor_id);
    buf.AppendFixed<double>(temperature);
    buf.AppendFixed<uint32_t>(static_cast<uint32_t>(label.size()));
    buf.Append(reinterpret_cast<const uint8_t*>(label.data()), label.size());
}

struct Args {
    std::string bind_address = "127.0.0.1";
    uint16_t    port = 9091;
    int         heartbeat_ms = 100;
};

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--heartbeat-ms" && i + 1 < argc) {
            a.heartbeat_ms = std::stoi(argv[++i]);
        } else if (arg == "--bind-address" && i + 1 < argc) {
            a.bind_address = argv[++i];
        }
    }
    return a;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args = ParseArgs(argc, argv);

    auto provider = std::make_shared<InProcessProvider>();
    auto driver = std::make_shared<fletcher::Driver>(provider);

    driver->CreateTopic({"heartbeat"}, BuildSensorSchema());
    driver->CreateTopic({"echo"},      BuildSensorSchema());

    fletcher::WebGatewayOptions opts;
    opts.address = args.bind_address;
    opts.port    = args.port;

    fletcher::WebGateway gw(driver, opts);
    gw.Start();

    std::printf("READY %u\n", args.port);
    std::fflush(stdout);

    std::atomic<bool> running{true};
    std::thread heartbeat;
    if (args.heartbeat_ms > 0) {
        heartbeat = std::thread([&] {
            int32_t seq = 0;
            while (running.load(std::memory_order_relaxed)) {
                driver->Publish({"heartbeat"},
                    [seq](fletcher::WriteBuffer& buf) {
                        EncodeSensorRow(buf,
                                        seq,
                                        static_cast<double>(seq) * 1.5,
                                        "hb");
                    });
                seq++;
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(args.heartbeat_ms));
            }
        });
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }

    running.store(false, std::memory_order_relaxed);
    if (heartbeat.joinable()) {
        heartbeat.join();
    }
    gw.Stop();
    return 0;
}
