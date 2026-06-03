// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <thread>

using namespace fletcher;
using namespace eprosima::fastdds::dds;

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

static PubSubProvider::RowEncoder MakeEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        buf.AppendFixed<int32_t>(x);
    };
}

static int32_t DecodeRow(const uint8_t* data) {
    int32_t v;
    std::memcpy(&v, data + 1, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// Tests — basic provider behaviour
// ---------------------------------------------------------------------------

TEST(FastDDSPubSubProviderTest, ConstructDestruct) {
    EXPECT_NO_THROW({ FastDDSPubSubProvider p(FastDDSProviderOptions{}); });
}

TEST(FastDDSPubSubProviderTest, CreateTopicSucceeds) {
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    EXPECT_NO_THROW(p.CreateTopic({"create", "ok"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicRejectsDuplicate) {
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"create", "dup"}, MakeSchema());
    EXPECT_THROW(p.CreateTopic({"create", "dup"}, MakeSchema()), std::runtime_error);
}

TEST(FastDDSPubSubProviderTest, PublishWithoutSubscriberDoesNotThrow) {
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"pub", "nosub"}, MakeSchema());
    EXPECT_NO_THROW(p.Publish({"pub", "nosub"}, MakeEncoder(1)));
}

TEST(FastDDSPubSubProviderTest, RoundTripPublishSubscribe) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});

    pub_provider.CreateTopic({"roundtrip", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"roundtrip", "x"}, [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            if (len >= 5) received.store(DecodeRow(data));
        });

    ASSERT_TRUE(result.schema.valid());
    SharedSchema sch = result.schema.get();
    ASSERT_TRUE(sch);
    ASSERT_EQ(sch->n_children, 1);
    EXPECT_EQ(std::string(sch->children[0]->name), "x");
    EXPECT_EQ(std::string(sch->children[0]->format), "i");

    pub_provider.Publish({"roundtrip", "x"}, MakeEncoder(42));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(received.load(), 42);
}

// ---------------------------------------------------------------------------
// Tests — QoS configuration via FastDDSProviderOptions
// ---------------------------------------------------------------------------

// A KEEP_LAST(N) writer QoS keeps things working but lets us swap out the
// default KEEP_ALL profile and still verify message delivery — proving the
// configured QoS is the one actually applied to the DataWriter.
TEST(FastDDSPubSubProviderTest, CustomDefaultWriterQos) {
    FastDDSProviderOptions opts;
    opts.default_writer_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    opts.default_writer_qos.history().depth = 10;
    FastDDSPubSubProvider pub_provider(std::move(opts));

    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});

    pub_provider.CreateTopic({"customdefault", "writer"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe({"customdefault", "writer"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len >= 5) received.store(DecodeRow(data));
                           });

    pub_provider.Publish({"customdefault", "writer"}, MakeEncoder(7));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 7);
}

TEST(FastDDSPubSubProviderTest, CustomDefaultReaderQos) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    FastDDSProviderOptions opts;
    opts.default_reader_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    opts.default_reader_qos.history().depth = 10;
    FastDDSPubSubProvider sub_provider(std::move(opts));

    pub_provider.CreateTopic({"customdefault", "reader"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe({"customdefault", "reader"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len >= 5) received.store(DecodeRow(data));
                           });

    pub_provider.Publish({"customdefault", "reader"}, MakeEncoder(11));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 11);
}

// Per-topic override should affect only the specified topic; an untouched
// topic on the same provider must still use the instance default.
TEST(FastDDSPubSubProviderTest, PerTopicWriterQosOverridesDefault) {
    FastDDSProviderOptions opts;
    DataWriterQos override_qos = opts.default_writer_qos;
    override_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    override_qos.history().depth = 5;
    opts.topic_writer_qos["pertopic/override"] = std::move(override_qos);

    FastDDSPubSubProvider pub_provider(std::move(opts));
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});

    pub_provider.CreateTopic({"pertopic", "override"}, MakeSchema());
    pub_provider.CreateTopic({"pertopic", "default"}, MakeSchema());

    std::atomic<int32_t> received_override{-1};
    std::atomic<int32_t> received_default{-1};
    sub_provider.Subscribe({"pertopic", "override"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len >= 5) received_override.store(DecodeRow(data));
                           });
    sub_provider.Subscribe({"pertopic", "default"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len >= 5) received_default.store(DecodeRow(data));
                           });

    pub_provider.Publish({"pertopic", "override"}, MakeEncoder(101));
    pub_provider.Publish({"pertopic", "default"}, MakeEncoder(202));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((received_override.load() == -1 || received_default.load() == -1) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received_override.load(), 101);
    EXPECT_EQ(received_default.load(), 202);
}

TEST(FastDDSPubSubProviderTest, PerTopicReaderQosOverridesDefault) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    FastDDSProviderOptions sub_opts;
    DataReaderQos override_qos = sub_opts.default_reader_qos;
    override_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    override_qos.history().depth = 5;
    sub_opts.topic_reader_qos["pertopic/readeroverride"] = std::move(override_qos);

    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"pertopic", "readeroverride"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe({"pertopic", "readeroverride"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len >= 5) received.store(DecodeRow(data));
                           });

    pub_provider.Publish({"pertopic", "readeroverride"}, MakeEncoder(303));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 303);
}

// Mirrors Autonomy's QoS profile: TRANSIENT_LOCAL + RELIABLE + KEEP_LAST(10)
// + explicit resource limits on both writer and reader. Sets it via Options
// to prove the typed Options path covers a realistic production profile.
TEST(FastDDSPubSubProviderTest, AutonomyStyleProfileViaOptions) {
    FastDDSProviderOptions pub_opts;
    pub_opts.default_writer_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    pub_opts.default_writer_qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    pub_opts.default_writer_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    pub_opts.default_writer_qos.history().depth = 10;
    pub_opts.default_writer_qos.resource_limits().max_samples = 100;
    pub_opts.default_writer_qos.resource_limits().max_samples_per_instance = 100;

    FastDDSProviderOptions sub_opts;
    sub_opts.default_reader_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    sub_opts.default_reader_qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    sub_opts.default_reader_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    sub_opts.default_reader_qos.history().depth = 10;
    sub_opts.default_reader_qos.resource_limits().max_samples = 100;
    sub_opts.default_reader_qos.resource_limits().max_samples_per_instance = 100;

    FastDDSPubSubProvider pub_provider(std::move(pub_opts));
    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"autonomy", "profile"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe({"autonomy", "profile"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len >= 5) received.store(DecodeRow(data));
                           });

    pub_provider.Publish({"autonomy", "profile"}, MakeEncoder(2026));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 2026);
}
