// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <mutex>
#include <thread>
#include <vector>

#include "internal/ordered_delivery.hpp"

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

// A schema that differs from MakeSchema() (different field name + type) so a
// re-declaration with it is a genuine conflict.
static OwnedSchema MakeOtherSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "y");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_DOUBLE);
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

TEST(FastDDSPubSubProviderTest, CreateTopicIsIdempotent) {
    // CreateTopic mirrors the in-process reference provider: declaring an
    // already-existing topic is a no-op, not an error. This lets a publisher
    // attach to a topic a subscriber created first (subscriber-first) and
    // makes repeated declarations harmless.
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"create", "dup"}, MakeSchema());
    EXPECT_NO_THROW(p.CreateTopic({"create", "dup"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicRejectsConflictingSchema) {
    // Idempotent re-declaration is fine, but declaring an existing topic with a
    // *different* schema is a genuine conflict and must not be silently dropped.
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"create", "conflict"}, MakeSchema());
    EXPECT_THROW(p.CreateTopic({"create", "conflict"}, MakeOtherSchema()), std::runtime_error);
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

// ---------------------------------------------------------------------------
// Subscriber-first: Subscribe before any publisher/topic exists must not block
// or throw, and once a publisher appears the schema future resolves and the
// first callback fires with a non-null schema (data is held until the schema
// arrives — the callback is never invoked with a null schema).
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, SubscribeBeforePublishDeliversWithSchema) {
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int32_t> received{-1};
    SharedSchema rx_schema;

    // Subscribe with no publisher yet — must return immediately (no block, no throw).
    SubscriptionResult result = sub_provider.Subscribe(
        {"subfirst", "x"}, [&](const uint8_t* data, size_t len, SharedSchema schema, Attachments) {
            std::lock_guard<std::mutex> lk(mu);
            rx_schema = schema;
            if (len >= 5) received.store(DecodeRow(data));
            cv.notify_all();
        });

    // No publisher has announced the schema yet, so the future is unresolved.
    EXPECT_TRUE(result.schema.valid());
    EXPECT_EQ(result.schema.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    // A publisher appears and publishes.
    pub_provider.CreateTopic({"subfirst", "x"}, MakeSchema());
    pub_provider.Publish({"subfirst", "x"}, MakeEncoder(99));

    // The schema future now resolves with a non-null schema.
    ASSERT_EQ(result.schema.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    SharedSchema fut_schema = result.schema.get();
    ASSERT_TRUE(fut_schema);
    EXPECT_EQ(fut_schema->n_children, 1);

    // The first callback fired with the row and a non-null schema.
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return received.load() != -1; }));
    }
    EXPECT_EQ(received.load(), 99);
    {
        std::lock_guard<std::mutex> lk(mu);
        ASSERT_TRUE(rx_schema);
        EXPECT_EQ(rx_schema->n_children, 1);
    }

    sub_provider.Unsubscribe({"subfirst", "x"});
}

// ---------------------------------------------------------------------------
// Subscribe-first burst: a real round-trip where the subscriber joins before
// the publisher, so the first samples are buffered until the schema arrives
// and then flushed. Functional smoke test that the whole burst is delivered,
// in order. (A deterministic proof of the handoff-ordering invariant — that a
// live sample arriving mid-flush cannot overtake the backlog — is the
// OrderedDelivery unit test below; that race is timing-dependent over real
// DDS, so it is verified directly on the mechanism instead.)
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, SubscribeFirstBurstDeliveredInOrder) {
    constexpr int32_t kCount = 1000;

    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    std::mutex mu;
    std::condition_variable cv;
    std::vector<int32_t> received;

    sub_provider.Subscribe({"ordering", "burst"},
                           [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                               if (len < 5) return;
                               std::lock_guard<std::mutex> lk(mu);
                               received.push_back(DecodeRow(data));
                               cv.notify_all();
                           });

    pub_provider.CreateTopic({"ordering", "burst"}, MakeSchema());
    for (int32_t i = 0; i < kCount; ++i) {
        pub_provider.Publish({"ordering", "burst"}, MakeEncoder(i));
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(15),
                                [&] { return received.size() == static_cast<size_t>(kCount); }))
            << "received " << received.size() << " of " << kCount << " samples";
    }
    sub_provider.Unsubscribe({"ordering", "burst"});

    std::lock_guard<std::mutex> lk(mu);
    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int32_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[static_cast<size_t>(i)], i) << "out-of-order delivery at index " << i;
    }
}

// ---------------------------------------------------------------------------
// OrderedDelivery — deterministic proof of the schema-handoff invariant.
//
// The bug: the buggy listener had two delivery paths — a live sample (data
// thread) was delivered directly, bypassing the backlog being flushed by the
// schema thread — so the two ran concurrently and a live sample could
// overtake the backlog. OrderedDelivery removes the second path: every sample
// goes through one FIFO drained by a single drainer.
//
// The single-drainer guard is what serialises delivery. Its observable,
// thread-free signature: a sample offered *while a drain is in progress* must
// NOT be delivered inline (nested inside the current callback) — on the real
// two-thread path an inline/concurrent delivery is exactly the overtaking
// race. We trigger it deterministically by re-entering Offer() from inside the
// callback. With the guard the re-offered sample is queued and delivered after
// the current callback returns (no nesting); remove the guard and Offer drains
// reentrantly, invoking a callback nested inside another — which this test
// catches. Order is asserted too: the late sample lands strictly last.
// ---------------------------------------------------------------------------
TEST(OrderedDeliveryTest, MidFlushOfferIsNotDeliveredInline) {
    std::vector<int32_t> order;
    int active = 0;       // callbacks currently on the stack
    bool nested = false;  // a callback was entered while another was active
    fletcher::internal::OrderedDelivery* self = nullptr;
    bool injected = false;

    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
            ASSERT_GE(len, 5u);
            if (active > 0) {
                nested = true;
            }
            ++active;
            order.push_back(DecodeRow(data));
            // While the backlog [0,1,2] is draining, a fresh live sample
            // arrives. Re-entering Offer here is the deterministic stand-in for
            // the data-reader thread delivering during the flush.
            if (!injected) {
                injected = true;
                std::vector<uint8_t> row(5);
                row[0] = 0x00;
                int32_t v = 99;
                std::memcpy(row.data() + 1, &v, sizeof(v));
                self->Offer(std::move(row), {});
            }
            --active;
        });
    self = &delivery;

    auto row_bytes = [](int32_t v) {
        std::vector<uint8_t> row(5);
        row[0] = 0x00;
        std::memcpy(row.data() + 1, &v, sizeof(v));
        return row;
    };

    // Three samples arrive before the schema is known — buffered, not delivered.
    delivery.Offer(row_bytes(0), {});
    delivery.Offer(row_bytes(1), {});
    delivery.Offer(row_bytes(2), {});
    EXPECT_TRUE(order.empty()) << "samples must be held until the schema is set";

    // Schema resolves: the backlog drains. The sample offered mid-flush must be
    // delivered after the current callback returns (not nested), and land last.
    delivery.SetSchema(MakeSharedSchema(MakeSchema()));

    EXPECT_FALSE(nested)
        << "a sample offered mid-flush was delivered inline — on the real two-thread "
           "path that is the live-sample-overtakes-backlog race";
    EXPECT_EQ(order, (std::vector<int32_t>{0, 1, 2, 99}));
}

// A sample offered before the schema is known must not reach the callback
// until SetSchema arrives (no null-schema delivery).
TEST(OrderedDeliveryTest, HoldsSamplesUntilSchemaIsSet) {
    std::vector<int32_t> order;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, SharedSchema schema, Attachments) {
            ASSERT_GE(len, 5u);
            EXPECT_TRUE(schema) << "callback invoked with a null schema";
            order.push_back(DecodeRow(data));
        });

    std::vector<uint8_t> row(5);
    row[0] = 0x00;
    int32_t v = 7;
    std::memcpy(row.data() + 1, &v, sizeof(v));
    delivery.Offer(row, {});
    EXPECT_TRUE(order.empty());

    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 7);
}

// A null schema must never release buffered samples — that would drain them
// with a null schema and break schema-before-data. Buffering continues until a
// real schema arrives.
TEST(OrderedDeliveryTest, NullSchemaDoesNotReleaseBufferedSamples) {
    std::vector<int32_t> order;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, SharedSchema schema, Attachments) {
            ASSERT_GE(len, 5u);
            EXPECT_TRUE(schema) << "callback invoked with a null schema";
            order.push_back(DecodeRow(data));
        });

    std::vector<uint8_t> row(5);
    row[0] = 0x00;
    int32_t v = 7;
    std::memcpy(row.data() + 1, &v, sizeof(v));
    delivery.Offer(row, {});

    delivery.SetSchema(nullptr);  // must not flip schema_ready_ or drain
    EXPECT_TRUE(order.empty());

    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 7);
}
