// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <thread>

using namespace eprosima::fastdds::dds;

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
        buf.AppendByte(0x00);  // null bitfield: no nulls
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
        {"roundtrip", "x"}, [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) received.store(DecodeRow(data));
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

// ---------------------------------------------------------------------------
// Custom QoS tests
// ---------------------------------------------------------------------------

TEST(FastDDSPubSubProviderTest, CustomWriterQosKeepLast) {
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 10;

    pub_provider.CreateTopic({"qos", "writer"}, MakeSchema(), wqos);

    std::atomic<int32_t> received{-1};
    auto result = sub_provider.Subscribe(
        {"qos", "writer"}, [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) received.store(DecodeRow(data));
        });

    ASSERT_TRUE(result.schema.valid());

    pub_provider.Publish({"qos", "writer"}, MakeEncoder(99));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(received.load(), 99);
}

TEST(FastDDSPubSubProviderTest, CustomReaderQosVolatile) {
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    pub_provider.CreateTopic({"qos", "reader"}, MakeSchema());

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.durability().kind = VOLATILE_DURABILITY_QOS;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 10;

    std::atomic<int32_t> received{-1};
    auto result = sub_provider.Subscribe(
        {"qos", "reader"},
        [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) received.store(DecodeRow(data));
        },
        rqos);

    ASSERT_TRUE(result.schema.valid());

    pub_provider.Publish({"qos", "reader"}, MakeEncoder(77));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(received.load(), 77);
}

TEST(FastDDSPubSubProviderTest, AutonomyStyleQos) {
    // Mirrors the common Autonomy QoS profile:
    //   Writer: TRANSIENT_LOCAL + RELIABLE + KEEP_LAST(10)
    //   Reader: VOLATILE + RELIABLE + KEEP_LAST(10)
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 10;
    wqos.resource_limits().max_samples = 10;
    wqos.resource_limits().max_instances = 1;
    wqos.resource_limits().max_samples_per_instance = 10;

    pub_provider.CreateTopic({"qos", "autonomy"}, MakeSchema(), wqos);

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.durability().kind = VOLATILE_DURABILITY_QOS;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 10;
    rqos.resource_limits().max_samples = 10;
    rqos.resource_limits().max_instances = 1;
    rqos.resource_limits().max_samples_per_instance = 10;
    rqos.resource_limits().allocated_samples = 10;

    std::atomic<int32_t> received{-1};
    auto result = sub_provider.Subscribe(
        {"qos", "autonomy"},
        [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) received.store(DecodeRow(data));
        },
        rqos);

    ASSERT_TRUE(result.schema.valid());

    pub_provider.Publish({"qos", "autonomy"}, MakeEncoder(55));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(received.load(), 55);
}

TEST(FastDDSPubSubProviderTest, UnrelatedConfigTypeUsesDefaults) {
    // Passing a non-QoS type in std::any should be silently ignored,
    // falling back to the built-in defaults.
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    pub_provider.CreateTopic({"qos", "fallback"}, MakeSchema(), std::any(42));

    std::atomic<int32_t> received{-1};
    auto result = sub_provider.Subscribe(
        {"qos", "fallback"},
        [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) received.store(DecodeRow(data));
        },
        std::any(std::string("not a qos")));

    ASSERT_TRUE(result.schema.valid());

    pub_provider.Publish({"qos", "fallback"}, MakeEncoder(33));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(received.load(), 33);
}

TEST(FastDDSPubSubProviderTest, CustomWriterAndReaderQosCombined) {
    FastDDSPubSubProvider pub_provider;
    FastDDSPubSubProvider sub_provider;

    DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
    wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    wqos.history().kind = KEEP_LAST_HISTORY_QOS;
    wqos.history().depth = 5;

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    rqos.history().kind = KEEP_LAST_HISTORY_QOS;
    rqos.history().depth = 5;

    pub_provider.CreateTopic({"qos", "both"}, MakeSchema(), wqos);

    std::atomic<int32_t> last_received{-1};
    std::atomic<int> count{0};
    auto result = sub_provider.Subscribe(
        {"qos", "both"},
        [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) {
                last_received.store(DecodeRow(data));
                count.fetch_add(1);
            }
        },
        rqos);

    ASSERT_TRUE(result.schema.valid());

    for (int i = 1; i <= 3; ++i) pub_provider.Publish({"qos", "both"}, MakeEncoder(i * 10));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (count.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(count.load(), 3);
    EXPECT_EQ(last_received.load(), 30);
}
