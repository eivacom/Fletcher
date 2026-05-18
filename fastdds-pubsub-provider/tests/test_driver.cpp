// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <fast_dds_pubsub_provider.hpp>
#include <core/write_buffer.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

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
        buf.AppendByte(0x00);          // null bitfield: no nulls
        buf.AppendFixed<int32_t>(x);
    };
}

static int32_t DecodeRow(const uint8_t* data) {
    int32_t v;
    std::memcpy(&v, data + 1, sizeof(v));  // skip 1-byte null bitfield
    return v;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(FastDDSPubSubProviderTest, ConstructDestruct) {
    EXPECT_NO_THROW({ FastDDSPubSubProvider p; });
}

TEST(FastDDSPubSubProviderTest, CreateTopicSucceeds) {
    FastDDSPubSubProvider p;
    EXPECT_NO_THROW(p.CreateTopic({"create", "ok"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicRejectsDuplicate) {
    FastDDSPubSubProvider p;
    p.CreateTopic({"create", "dup"}, MakeSchema());
    EXPECT_THROW(p.CreateTopic({"create", "dup"}, MakeSchema()), std::runtime_error);
}

TEST(FastDDSPubSubProviderTest, PublishWithoutSubscriberDoesNotThrow) {
    FastDDSPubSubProvider p;
    p.CreateTopic({"pub", "nosub"}, MakeSchema());
    EXPECT_NO_THROW(p.Publish({"pub", "nosub"}, MakeEncoder(1)));
}

TEST(FastDDSPubSubProviderTest, RoundTripPublishSubscribe) {
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    pub_provider.CreateTopic({"roundtrip", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    auto result = sub_provider.Subscribe(
        {"roundtrip", "x"},
        [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5)
                received.store(DecodeRow(data));
        });

    // Subscribe internally polls until the schema arrives, so once it returns
    // we know the schema topic matched. Verify the returned schema is correct.
    ASSERT_TRUE(result.schema.valid());
    ASSERT_EQ(result.schema->n_children, 1);
    EXPECT_EQ(std::string(result.schema->children[0]->name), "x");
    EXPECT_EQ(std::string(result.schema->children[0]->format), "i");  // int32

    // Publish — the DataWriter is TRANSIENT_LOCAL + KEEP_ALL, so the sample
    // is retained and delivered once the data-topic DataReader matches.
    pub_provider.Publish({"roundtrip", "x"}, MakeEncoder(42));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(received.load(), 42);
}
