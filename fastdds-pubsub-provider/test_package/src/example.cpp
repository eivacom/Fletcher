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

static PubSubProvider::RowEncoder MakeEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        buf.AppendFixed<int32_t>(x);
    };
}

int main() {
    // ProviderConfig and nothing else — and this TU is the machine check for that: the package
    // recipe drops `transitive_headers`, so it compiles with no Fast DDS include directories at
    // all. An eProsima type surviving in the installed header would be a compile error HERE
    // (PDA-DEC-6 §5, owner ruling 2026-08-31 "Fletcher never learns DDS vocabulary").
    FastDDSPubSubProvider pub_provider(ProviderConfig{});
    FastDDSPubSubProvider sub_provider(ProviderConfig{});

    pub_provider.CreateTopic({"example", "topic"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"example", "topic"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) {
                int32_t v;
                std::memcpy(&v, data + 1, sizeof(v));
                received.store(v);
            }
        });

    // One waiting mechanism, with a deadline and a typed outcome.
    SharedSchema schema;
    if (result.schema.Wait(std::chrono::seconds(5), &schema) != PubSubStatus::kOk || !schema) {
        std::fputs("FAIL: schema not received from publisher\n", stderr);
        return 1;
    }

    pub_provider.Publish({"example", "topic"}, MakeEncoder(42));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (received.load() != 42) {
        std::fprintf(stderr, "FAIL: expected 42, got %d\n", received.load());
        return 1;
    }

    std::fputs("PASS: round-trip publish/subscribe OK\n", stdout);
    return 0;
}
