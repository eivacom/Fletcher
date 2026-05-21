// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <thread>

using namespace fletcher;

static OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

static PubSub::RowEncoder MakeEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        buf.AppendFixed<int32_t>(x);
    };
}

int main() {
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    pub_provider.CreateTopic({"example", "topic"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    auto result = sub_provider.Subscribe(
        {"example", "topic"}, [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) {
                int32_t v;
                std::memcpy(&v, data + 1, sizeof(v));
                received.store(v);
            }
        });

    if (!result.schema.valid()) {
        std::fputs("FAIL: schema not received from publisher\n", stderr);
        return 1;
    }

    pub_provider.Publish({"example", "topic"}, MakeEncoder(42));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (received.load() != 42) {
        std::fprintf(stderr, "FAIL: expected 42, got %d\n", received.load());
        return 1;
    }

    std::fputs("PASS: round-trip publish/subscribe OK\n", stdout);
    return 0;
}
