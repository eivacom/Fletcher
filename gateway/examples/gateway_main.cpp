// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Minimal smoke test for the WebGateway.
//
// Starts a WebSocket server on port 9090 with an in-process mock provider.
// Creates a topic "demo/telemetry", then publishes a test row every
// second so that a connecting client can subscribe and see data flowing.
//
// Test with any WebSocket client, e.g.:
//   wscat -c ws://localhost:9090
// Then send a JSON text frame:
//   {"action":"subscribe","topic":"demo/telemetry"}

#include <web_gateway/web_gateway.hpp>
#include <pubsub/driver.hpp>
#include <pubsub/pubsub.hpp>
#include <pubsub/owned_schema.hpp>
#include <core/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Simple in-process mock provider (no DDS dependency).
// ---------------------------------------------------------------------------

class InProcessProvider : public fletcher::PubSub {
 public:
    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema /*schema*/,
                     std::any /*config*/) override {
        std::lock_guard lock(mu_);
        auto key = Join(segments);
        topics_[key] = {};
    }

    void Publish(const std::vector<std::string>& segments,
                 RowEncoder encoder,
                 const fletcher::Attachments& attachments) override {
        // Encode into a temporary buffer.
        std::vector<uint8_t> buf;
        fletcher::VectorWriteBuffer wb(buf);
        encoder(wb);

        SubscribeCallback cb;
        {
            std::lock_guard lock(mu_);
            auto it = topics_.find(Join(segments));
            if (it == topics_.end()) return;
            cb = it->second;
        }
        if (cb) cb(buf.data(), buf.size(), nullptr, attachments);
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
        if (it != topics_.end())
            it->second = nullptr;
    }

 private:
    std::mutex mu_;
    std::unordered_map<std::string, SubscribeCallback> topics_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) out += '/';
            out += segs[i];
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    // Set up provider -> driver -> gateway.
    auto provider = std::make_shared<InProcessProvider>();
    auto driver   = std::make_shared<fletcher::Driver>(provider);

    // Build a simple schema via nanoarrow: struct{ seq: uint32 }.
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "seq");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_UINT32);

    std::vector<std::string> topic = {"demo", "telemetry"};
    driver->CreateTopic(topic, std::move(schema));

    fletcher::WebGatewayOptions opts;
    opts.port = 9090;

    fletcher::WebGateway gw(driver, opts);
    gw.Start();

    std::printf("WebGateway listening on ws://localhost:%d\n", opts.port);
    std::printf("Topic: demo/telemetry\n");
    std::printf("Press Ctrl+C to stop.\n\n");

    // Publish a test row every second.
    std::atomic<bool> running{true};
    std::thread publisher([&] {
        uint32_t seq = 0;
        while (running) {
            driver->Publish(topic,
                [seq](fletcher::WriteBuffer& buf) {
                    buf.AppendByte(0x00);              // null bitfield: not null
                    buf.AppendFixed<uint32_t>(seq);
                });

            std::printf("Published seq=%u\n", seq);
            seq++;

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    // Wait for Ctrl+C (or just sleep forever).
    // On Windows, Ctrl+C terminates the process; the destructor handles cleanup.
    publisher.join();

    gw.Stop();
    return 0;
}
