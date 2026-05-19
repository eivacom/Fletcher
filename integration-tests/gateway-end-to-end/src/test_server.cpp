// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Test fixture for the gateway end-to-end integration test.
//
// Brings up a WebGateway on a CLI-given port, backed by an
// InProcessProvider that mirrors the pattern in gateway/examples/
// gateway_main.cpp. Pre-creates two topics:
//
//   * "heartbeat" — the server periodically publishes a synthetic row
//     to this topic so the vitest test can verify the
//     publish-from-server -> client-delivery path.
//   * "echo"      — the test client publishes to this topic. Because
//     the InProcessProvider routes Publish() to the registered
//     subscriber callback, any sample the client publishes is
//     delivered back to the same client on a subscription it took
//     out beforehand. That gives us the client-publish ->
//     server-delivery path without needing a side channel.
//
// Once the gateway is accepting connections the server prints
//   READY <port>
// on stdout so the test can synchronise without polling the port.
// It then reads stdin and exits cleanly on "stop" — gives the
// test deterministic shutdown without relying on SIGTERM ordering
// (important on Windows where SIGTERM semantics differ).

#include <web_gateway/web_gateway.hpp>
#include <pubsub/driver.hpp>
#include <pubsub/pubsub.hpp>
#include <pubsub/owned_schema.hpp>
#include <core/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
// InProcessProvider — same pattern as gateway/examples/gateway_main.cpp.
// Single callback per topic; Publish() invokes it synchronously.
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

// ─────────────────────────────────────────────────────────────────────
// Build the {sensor_id:int32, temperature:float64, label:utf8} schema
// that both heartbeat and echo topics share.
// ─────────────────────────────────────────────────────────────────────
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

// Encode a single sensor row into the positional wire format used by
// the codec on both sides: [null_bitfield :1][int32 :4][float64 :8]
// [utf8_len :4][utf8 :N].
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
    uint16_t port = 9091;
    int heartbeat_ms = 100;
};

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--heartbeat-ms" && i + 1 < argc) {
            a.heartbeat_ms = std::stoi(argv[++i]);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args = ParseArgs(argc, argv);

    auto provider = std::make_shared<InProcessProvider>();
    auto driver = std::make_shared<fletcher::Driver>(provider);

    // Pre-create both topics with the same schema so the client can
    // subscribe to either without supplying its own SchemaDescriptor.
    driver->CreateTopic({"heartbeat"}, BuildSensorSchema());
    driver->CreateTopic({"echo"},      BuildSensorSchema());

    fletcher::WebGatewayOptions opts;
    opts.address = "127.0.0.1";
    opts.port    = args.port;

    fletcher::WebGateway gw(driver, opts);
    gw.Start();

    // Synchronisation signal to the test runner.
    std::printf("READY %u\n", args.port);
    std::fflush(stdout);

    std::atomic<bool> running{true};
    std::thread heartbeat([&] {
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

    // Wait for the test runner to ask us to stop. Reading stdin gives
    // deterministic shutdown across platforms (Windows SIGTERM is not
    // a clean cooperative-cancel mechanism).
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }

    running.store(false, std::memory_order_relaxed);
    heartbeat.join();
    gw.Stop();
    return 0;
}
