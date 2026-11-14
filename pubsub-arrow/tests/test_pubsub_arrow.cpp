// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/schema_import.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Mock provider (nanoarrow interface)
// ---------------------------------------------------------------------------

class MockProvider : public PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) override {
        std::string key = fletcher::internal::JoinSegments(segments);
        topics_created.push_back(key);
        if (schema) {
            schemas_[key] = OwnedSchema::DeepCopy(schema.get());
        }
    }

    void Publish(const std::vector<std::string>& segments, const RowEncoder& encoder,
                 const Attachments& attachments) override {
        std::string key = fletcher::internal::JoinSegments(segments);

        VectorWriteBuffer wb;
        encoder(wb);
        const std::vector<uint8_t> buf = wb.Finish();
        published.push_back(buf);

        auto it = callbacks_.find(key);
        if (it != callbacks_.end()) {
            SharedSchema sp;
            auto sit = schemas_.find(key);
            if (sit != schemas_.end()) {
                sp = MakeSharedSchema(OwnedSchema::DeepCopy(sit->second.get()));
            }
            it->second(buf.data(), buf.size(), sp, attachments);
        }
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback) override {
        std::string key = fletcher::internal::JoinSegments(segments);
        callbacks_[key] = std::move(callback);
        auto it = schemas_.find(key);
        SharedSchema schema;
        if (it != schemas_.end()) {
            schema = MakeSharedSchema(OwnedSchema::DeepCopy(it->second.get()));
        }
        return {SchemaArrival::Ready(std::move(schema))};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(fletcher::internal::JoinSegments(segments));
    }

    // Recorded rather than refused: this MockProvider supports per-topic options, so the
    // forwarding tests below can observe what the Arrow-tier overloads pass through.
    void CreateTopicWithOptions(const std::vector<std::string>& segments, OwnedSchema schema,
                                const TopicOptions& options) override {
        create_topic_with_options_count++;
        last_create_options = options;
        CreateTopic(segments, std::move(schema));
    }

    SubscriptionResult SubscribeWithOptions(const std::vector<std::string>& segments,
                                            SubscribeCallback callback,
                                            const TopicOptions& options) override {
        subscribe_with_options_count++;
        last_subscribe_options = options;
        return Subscribe(segments, std::move(callback));
    }

    std::vector<std::string> topics_created;
    // Raw bytes handed to the RowEncoder on each underlying Publish() call, one entry per call.
    std::vector<std::vector<uint8_t>> published;
    int create_topic_with_options_count = 0;
    int subscribe_with_options_count = 0;
    TopicOptions last_create_options;
    TopicOptions last_subscribe_options;

   private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::unordered_map<std::string, OwnedSchema> schemas_;
};

// A schema-carrying provider that never receives a data subscription in the
// test below — SubscribeSchema is the only thing exercised through it, so
// `subscribe_count` staying 0 is the whole point of the test.
class SchemaOnlyProvider : public PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) override {
        std::string key = fletcher::internal::JoinSegments(segments);
        if (schema) {
            schemas_[key] = OwnedSchema::DeepCopy(schema.get());
        }
    }

    void Publish(const std::vector<std::string>&, const RowEncoder&, const Attachments&) override {}

    SubscriptionResult Subscribe(const std::vector<std::string>&, SubscribeCallback) override {
        ++subscribe_count;
        return {SchemaArrival::Ready(nullptr)};
    }

    void Unsubscribe(const std::vector<std::string>&) override {}

    SchemaArrival SubscribeSchema(const std::vector<std::string>& segments) override {
        auto it = schemas_.find(fletcher::internal::JoinSegments(segments));
        SharedSchema schema;
        if (it != schemas_.end()) {
            schema = MakeSharedSchema(OwnedSchema::DeepCopy(it->second.get()));
        }
        return SchemaArrival::Ready(std::move(schema));
    }

    void UnsubscribeSchema(const std::vector<std::string>& /*segments*/) override {
        ++unsubscribe_schema_count;
    }

    int subscribe_count = 0;
    int unsubscribe_schema_count = 0;

   private:
    std::unordered_map<std::string, OwnedSchema> schemas_;
};

static auto TestSchema() {
    return arrow::schema({
        arrow::field("x", arrow::int32()),
        arrow::field("name", arrow::utf8()),
    });
}

static const std::vector<std::string> kTopic = {"test", "topic"};

// ---------------------------------------------------------------------------
// PublisherArrow tests
// ---------------------------------------------------------------------------

TEST(PublisherArrowTest, CreateTopicConvertsArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    pub.CreateTopic(kTopic, TestSchema());
    ASSERT_EQ(mock->topics_created.size(), 1);
    EXPECT_EQ(mock->topics_created[0], "test/topic");
}

TEST(PublisherArrowTest, ListTopics) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    EXPECT_TRUE(pub.ListTopics().empty());

    pub.CreateTopic(kTopic, TestSchema());

    std::vector<std::string> topics = pub.ListTopics();
    ASSERT_EQ(topics.size(), 1);
    EXPECT_EQ(topics[0], "test/topic");
}

TEST(PublisherArrowTest, CreateTopicForwardsTopicOptions) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    TopicOptions options{.profile = "reliable", .max_payload_bytes = 4096};
    pub.CreateTopic(kTopic, TestSchema(), options);

    EXPECT_EQ(mock->create_topic_with_options_count, 1);
    ASSERT_EQ(mock->topics_created.size(), 1u);
    EXPECT_EQ(mock->last_create_options, options);
}

TEST(PublisherArrowTest, PublishTypeMismatchThrowsToCaller) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    pub.CreateTopic(kTopic, arrow::schema({arrow::field("a", arrow::int32())}));

    ArrowRow row = {std::make_shared<arrow::StringScalar>("nope")};
    EXPECT_THROW(pub.Publish(kTopic, row), std::invalid_argument);

    EXPECT_TRUE(mock->published.empty());
}

TEST(PublisherArrowTest, PublishAfterATypeMismatchStillDelivers) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    auto schema = arrow::schema({arrow::field("a", arrow::int32())});
    pub.CreateTopic(kTopic, schema);

    ArrowRow bad_row = {std::make_shared<arrow::StringScalar>("nope")};
    EXPECT_THROW(pub.Publish(kTopic, bad_row), std::invalid_argument);
    EXPECT_TRUE(mock->published.empty());

    // The scratch buffer survives the throw: a correct publish right
    // after, on the same thread, is delivered intact.
    ArrowRow good_row = {std::make_shared<arrow::Int32Scalar>(7)};
    pub.Publish(kTopic, good_row);

    ASSERT_EQ(mock->published.size(), 1u);
    Codec codec(schema);
    ArrowRow decoded = codec.DecodeRow(mock->published[0].data(), mock->published[0].size());
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*good_row[0]));
}

TEST(PublisherArrowTest, PublishTwiceReusesScratchAndDeliversBothRows) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    auto schema = TestSchema();
    pub.CreateTopic(kTopic, schema);

    ArrowRow row1 = {std::make_shared<arrow::Int32Scalar>(1),
                     std::make_shared<arrow::StringScalar>("first")};
    ArrowRow row2 = {std::make_shared<arrow::Int32Scalar>(2),
                     std::make_shared<arrow::StringScalar>("second")};

    pub.Publish(kTopic, row1);
    pub.Publish(kTopic, row2);

    ASSERT_EQ(mock->published.size(), 2u);

    Codec codec(schema);
    ArrowRow decoded1 = codec.DecodeRow(mock->published[0].data(), mock->published[0].size());
    ArrowRow decoded2 = codec.DecodeRow(mock->published[1].data(), mock->published[1].size());

    ASSERT_EQ(decoded1.size(), 2u);
    EXPECT_TRUE(decoded1[0]->Equals(*row1[0]));
    EXPECT_TRUE(decoded1[1]->Equals(*row1[1]));
    ASSERT_EQ(decoded2.size(), 2u);
    EXPECT_TRUE(decoded2[0]->Equals(*row2[0]));
    EXPECT_TRUE(decoded2[1]->Equals(*row2[1]));
}

// Publisher::CreateTopic (publisher.cpp) treats a re-declaration with a byte-identical schema as
// a no-op and never calls back into the provider, so this exercises PublisherArrow's own map
// insert on the second CreateTopic call: it must not replace the existing codecs_ entry, because
// Publish() copies out a raw Codec* with mu_ released before encoding, and a concurrent
// redeclaration that replaced the entry would free a codec a running EncodeRow still points at.
// The race itself isn't reproducible deterministically here; this only pins that a redeclare
// leaves a codec that still round-trips.
TEST(PublisherArrowTest, RedeclaringATopicKeepsTheCodec) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    auto schema = TestSchema();
    pub.CreateTopic(kTopic, schema);
    pub.CreateTopic(kTopic, schema);  // re-declare with the identical schema

    ArrowRow row = {std::make_shared<arrow::Int32Scalar>(3),
                    std::make_shared<arrow::StringScalar>("third")};
    pub.Publish(kTopic, row);

    ASSERT_EQ(mock->published.size(), 1u);
    Codec codec(schema);
    ArrowRow decoded = codec.DecodeRow(mock->published[0].data(), mock->published[0].size());
    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_TRUE(decoded[0]->Equals(*row[0]));
    EXPECT_TRUE(decoded[1]->Equals(*row[1]));
}

// ---------------------------------------------------------------------------
// SubscriberArrow tests
// ---------------------------------------------------------------------------

TEST(SubscriberArrowTest, SubscribeReturnsArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());
    SubscriberArrow::SubscribeResult result = sub.Subscribe(kTopic, [](ArrowRow, Attachments) {});

    // Re-anchored: the Arrow tier hands back the seam's own arrival, and the
    // caller converts with the one safe public conversion.
    SharedSchema nano;
    ASSERT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &nano), PubSubStatus::kOk);
    std::shared_ptr<arrow::Schema> sch = ImportArrowSchema(nano);
    ASSERT_NE(sch, nullptr);
    EXPECT_EQ(sch->num_fields(), 2);
    EXPECT_EQ(sch->field(0)->name(), "x");
    EXPECT_TRUE(sch->field(0)->type()->Equals(*arrow::int32()));
    EXPECT_EQ(sch->field(1)->name(), "name");
    EXPECT_TRUE(sch->field(1)->type()->Equals(*arrow::utf8()));
}

TEST(SubscriberArrowTest, SubscribeSchemaYieldsAnImportableSchemaWithoutADataSubscription) {
    auto provider = std::make_shared<SchemaOnlyProvider>();
    PublisherArrow pub(provider);
    SubscriberArrow sub(provider);
    pub.CreateTopic(kTopic, TestSchema());

    SchemaArrival arrival = sub.SubscribeSchema(kTopic);

    SharedSchema nano;
    ASSERT_EQ(arrival.Wait(std::chrono::milliseconds(0), &nano), PubSubStatus::kOk);
    std::shared_ptr<arrow::Schema> schema = ImportArrowSchema(nano);
    ASSERT_TRUE(schema);
    EXPECT_TRUE(schema->Equals(*TestSchema()));

    // No data subscription was ever opened for this.
    EXPECT_EQ(provider->subscribe_count, 0);

    sub.UnsubscribeSchema(kTopic);
    EXPECT_EQ(provider->unsubscribe_schema_count, 1);
}

// Optional means optional: MockProvider never overrode SubscribeSchema, so the
// base class's default refusal is what SubscriberArrow forwards.
TEST(SubscriberArrowTest, SubscribeSchemaOnATransportWithoutOneThrowsNotSupported) {
    auto mock = std::make_shared<MockProvider>();
    SubscriberArrow sub(mock);

    try {
        static_cast<void>(sub.SubscribeSchema(kTopic));
        FAIL() << "expected PubSubError(kNotSupported)";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kNotSupported);
    }
}

TEST(SubscriberArrowTest, SubscribeForwardsTopicOptions) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    TopicOptions options{.profile = "reliable"};
    static_cast<void>(sub.Subscribe(kTopic, [](ArrowRow, Attachments) {}, options));

    EXPECT_EQ(mock->subscribe_with_options_count, 1);
    EXPECT_EQ(mock->last_subscribe_options, options);
}

// Optional means optional: SchemaOnlyProvider never overrode SubscribeWithOptions, so the base
// class's default refusal is what SubscriberArrow forwards for a non-empty options request.
TEST(SubscriberArrowTest, SubscribeWithOptionsOnATransportWithoutOneThrowsNotSupported) {
    auto provider = std::make_shared<SchemaOnlyProvider>();
    SubscriberArrow sub(provider);

    try {
        static_cast<void>(
            sub.Subscribe(kTopic, [](ArrowRow, Attachments) {}, TopicOptions{.profile = "x"}));
        FAIL() << "expected PubSubError(kNotSupported)";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kNotSupported);
    }
}

TEST(PubSubArrowTest, PublishSubscribeRoundtripWithArrowRow) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    ArrowRow received;
    static_cast<void>(
        sub.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); }));

    ArrowRow sent = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
    };
    pub.Publish(kTopic, sent);

    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*received[0]).value, 42);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*received[1]).value->ToString(), "hello");
}

TEST(PubSubArrowTest, PublishWithAttachments) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    Attachments received_att;
    static_cast<void>(
        sub.Subscribe(kTopic, [&](ArrowRow, const Attachments& att) { received_att = att; }));

    const std::vector<uint8_t> payload{0xDE, 0xAD};
    Blob blob{payload};
    Attachments sent;
    sent.Set("img", blob);

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pub.Publish(kTopic, row, sent);

    const Blob* arrived = received_att.Find("img");
    ASSERT_NE(arrived, nullptr);
    // The kept copy names the very bytes that were published (§3.2).
    EXPECT_EQ(arrived->data(), blob.data());
    EXPECT_EQ(arrived->size(), payload.size());
}

TEST(PubSubArrowTest, PublishDirectPassthrough) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    std::shared_ptr<arrow::Schema> schema = arrow::schema({arrow::field("x", arrow::int32())});
    pub.CreateTopic(kTopic, schema);

    ArrowRow received;
    static_cast<void>(
        sub.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); }));

    pub.PublishDirect(kTopic, [](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        int32_t val = 99;
        buf.Append(reinterpret_cast<const uint8_t*>(&val), sizeof(val));
    });

    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*received[0]).value, 99);
}

TEST(PubSubArrowTest, Unsubscribe) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    int count = 0;
    SubscriberArrow::SubscribeResult result =
        sub.Subscribe(kTopic, [&](ArrowRow, Attachments) { count++; });

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pub.Publish(kTopic, row);
    EXPECT_EQ(count, 1);

    sub.Unsubscribe(result.subscription_id);
    pub.Publish(kTopic, row);
    EXPECT_EQ(count, 1);
}

// ---------------------------------------------------------------------------
// Batched RecordBatch Subscribe — flush triggers, attachments, dropped rows
// ---------------------------------------------------------------------------

namespace {

using BatchStatus = SubscriberArrow::BatchStatus;

// Thread-safe sink for delivered batches. Every batch arrives on the subscription's own batch
// thread (D10), never on the publishing thread, so a test reads after sink.WaitFor, or after an
// Unsubscribe / ~SubscriberArrow from outside the callback (L9).
struct BatchSink {
    struct Delivery {
        int64_t num_rows;
        std::vector<Attachments> attachments;
        BatchStatus status;
        std::shared_ptr<arrow::RecordBatch> batch;
    };

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Delivery> deliveries;

    SubscriberArrow::RecordBatchCallback callback() {
        return [this](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments> att,
                      BatchStatus status) {
            std::lock_guard<std::mutex> lk(mu);
            deliveries.push_back({batch ? batch->num_rows() : -1, std::move(att), status, batch});
            cv.notify_all();
        };
    }

    bool WaitFor(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return deliveries.size() >= n; });
    }
};

ArrowRow MakeRow(int32_t x, const std::string& name) {
    return {std::make_shared<arrow::Int32Scalar>(x), std::make_shared<arrow::StringScalar>(name)};
}

}  // namespace

TEST(SubscriberArrowBatchTest, SubscribeForwardsTopicOptions) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    TopicOptions options{.profile = "reliable"};
    static_cast<void>(
        sub.Subscribe(kTopic, sink.callback(), SubscriberArrow::BatchOptions{}, options));

    EXPECT_EQ(mock->subscribe_with_options_count, 1);
    EXPECT_EQ(mock->last_subscribe_options, options);
}

TEST(SubscriberArrowBatchTest, FlushesAtRowLimit) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 3;
    opt.timeout = std::chrono::minutes(10);  // long, so only the count triggers
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    for (int i = 0; i < 3; ++i) pub.Publish(kTopic, MakeRow(i, "n"));

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 3);
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 3u);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 0);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kRowLimit);
}

TEST(SubscriberArrowBatchTest, FlushesAtTimeout) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;  // high, so only the timeout triggers
    opt.timeout = std::chrono::milliseconds(100);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    pub.Publish(kTopic, MakeRow(1, "a"));
    pub.Publish(kTopic, MakeRow(2, "b"));

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 2);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 0);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kTimeout);
}

TEST(SubscriberArrowBatchTest, ClosingFlushOnUnsubscribe) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::minutes(10);
    auto result = sub.Subscribe(kTopic, sink.callback(), opt);

    pub.Publish(kTopic, MakeRow(7, "x"));
    pub.Publish(kTopic, MakeRow(8, "y"));
    sub.Unsubscribe(result.subscription_id);  // flushes the partial batch

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 2);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kClosing);
}

TEST(SubscriberArrowBatchTest, AttachmentsAlignWithRows) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 2;
    opt.timeout = std::chrono::minutes(10);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    Attachments sent;
    sent.Set("img", Blob{std::vector<uint8_t>{0xBE, 0xEF}});
    pub.Publish(kTopic, MakeRow(1, "a"), sent);  // row 0 has an attachment
    pub.Publish(kTopic, MakeRow(2, "b"));        // row 1 has none

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    ASSERT_EQ(sink.deliveries[0].num_rows, 2);
    ASSERT_EQ(sink.deliveries[0].attachments.size(), 2u);
    EXPECT_NE(sink.deliveries[0].attachments[0].Find("img"), nullptr);
    EXPECT_TRUE(sink.deliveries[0].attachments[1].empty());
}

TEST(SubscriberArrowBatchTest, DroppedRowReportedAndAttachmentDiscarded) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::minutes(10);
    auto result = sub.Subscribe(kTopic, sink.callback(), opt);

    Blob blob{std::vector<uint8_t>{0x01}};
    Attachments good;
    good.Set("img", blob);
    Attachments orphan;
    orphan.Set("orphan", blob);
    pub.Publish(kTopic, MakeRow(1, "good"), good);  // decodes fine

    // A truncated buffer (just the 1-byte null bitfield) underruns when the
    // int32 field is read, so DecodeRow throws and the row is dropped — and
    // its attachment is discarded with it.
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); }, orphan);

    sub.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 1);             // only the good row
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 1u);  // dropped row's attachment gone
    EXPECT_NE(sink.deliveries[0].attachments[0].Find("img"), nullptr);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 1);
}

TEST(SubscriberArrowBatchTest, OnlyDroppedRowsStillDeliversEmptyBatch) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::milliseconds(100);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    // Only a malformed row arrives in this window.
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 0);  // zero-row batch (decision A)
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 0u);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 1);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kTimeout);
}

// ---------------------------------------------------------------------------
// D7F-1 -- rows lost before a usable schema are reported at close (L1, L3, L6, L7, L9, D9)
//
// A batched subscription that never receives a usable schema counts every row it is handed as
// dropped. At close it must deliver exactly one batch: reason kClosing, batch == nullptr,
// rows_dropped == rows sent, no attachments (a dropped row takes its attachment with it).
// max_rows and timeout keep both flush triggers out of the window, so the only possible
// delivery is the closing one. Results are read straight after the ending returns: a closing
// call from outside delivers everything before it returns (L9).
// ---------------------------------------------------------------------------

namespace {

constexpr int kSchemaLessRows = 3;

SubscriberArrow::BatchOptions NoTriggerBatchOptions() {
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::minutes(10);
    return opt;
}

// Attachments for the second of the three rows; the other two carry none.
Attachments OrphanAttachments() {
    Attachments att;
    att.Set("orphan", Blob{std::vector<uint8_t>{0x01}});
    return att;
}

// Sends kSchemaLessRows raw rows through the Publisher, one with an attachment, for a topic that
// was never declared (so PublisherArrow has no codec). The bytes are never decoded.
void PublishSchemaLessRows(PublisherArrow& pub) {
    const auto row = [](WriteBuffer& buf) { buf.AppendByte(0x00); };
    pub.PublishDirect(kTopic, row);
    pub.PublishDirect(kTopic, row, OrphanAttachments());
    pub.PublishDirect(kTopic, row);
}

// The same count of well-formed rows, one with an attachment, for a declared topic: the rows are
// fine and the schema exists at the topic; only this subscription never received it.
void PublishWellFormedRows(PublisherArrow& pub) {
    pub.Publish(kTopic, MakeRow(1, "a"));
    pub.Publish(kTopic, MakeRow(2, "b"), OrphanAttachments());
    pub.Publish(kTopic, MakeRow(3, "c"));
}

void ExpectOneNullClosingReportOfAllRows(BatchSink& sink) {
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    const auto& d = sink.deliveries[0];
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(d.batch, nullptr);
    EXPECT_EQ(d.status.rows_dropped, kSchemaLessRows);
    EXPECT_TRUE(d.attachments.empty());
}

}  // namespace

// F1: the topic is never declared, so the Mock hands every row a null schema.
TEST(SubscriberArrowBatchTest, NoUsableSchemaDropsReportedAtUnsubscribe) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    BatchSink sink;
    auto result = sub.Subscribe(kTopic, sink.callback(), NoTriggerBatchOptions());

    PublishSchemaLessRows(pub);
    sub.Unsubscribe(result.subscription_id);

    ExpectOneNullClosingReportOfAllRows(sink);
}

// F2: as F1, ended by destroying the SubscriberArrow; the sink outlives it.
TEST(SubscriberArrowBatchTest, NoUsableSchemaDropsReportedAtDestruction) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    BatchSink sink;
    {
        SubscriberArrow sub(mock);
        static_cast<void>(sub.Subscribe(kTopic, sink.callback(), NoTriggerBatchOptions()));
        PublishSchemaLessRows(pub);
    }

    ExpectOneNullClosingReportOfAllRows(sink);
}

// F3: the topic is declared only AFTER the subscriber joined. The in-process provider
// (schema_carriage=as_declared) latches a subscription's schema once, at Subscribe, so this
// subscription's schema stays null for life although the topic now has one.
TEST(SubscriberArrowBatchTest, SchemaDeclaredAfterSubscribeDropsReportedAtUnsubscribe) {
    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    SubscriberArrow sub(provider);

    BatchSink sink;
    auto result = sub.Subscribe(kTopic, sink.callback(), NoTriggerBatchOptions());

    pub.CreateTopic(kTopic, TestSchema());
    PublishWellFormedRows(pub);
    sub.Unsubscribe(result.subscription_id);

    ExpectOneNullClosingReportOfAllRows(sink);
}

// F4: as F3, ended by destroying the SubscriberArrow; the sink outlives it.
TEST(SubscriberArrowBatchTest, SchemaDeclaredAfterSubscribeDropsReportedAtDestruction) {
    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);

    BatchSink sink;
    {
        SubscriberArrow sub(provider);
        static_cast<void>(sub.Subscribe(kTopic, sink.callback(), NoTriggerBatchOptions()));

        pub.CreateTopic(kTopic, TestSchema());
        PublishWellFormedRows(pub);
    }

    ExpectOneNullClosingReportOfAllRows(sink);
}

// ---------------------------------------------------------------------------
// D7F-2 -- rows lost before a usable schema are reported at each deadline (L1, L5, L6, L7, L8, L13)
//
// A batched subscription that is still open and has no usable schema must report its loss at the
// window's deadline, once per window, not only when it closes: reason kTimeout, batch == nullptr,
// rows_dropped == the window's rows. Reads go through sink.WaitFor, or happen after an outside
// Unsubscribe (L9), so none of this depends on which thread delivers (L12).
// ---------------------------------------------------------------------------

namespace {

constexpr auto kDeadlineTimeout = std::chrono::milliseconds(100);
constexpr int kWellFormedRows = 3;  // what PublishWellFormedRows sends

SubscriberArrow::BatchOptions DeadlineBatchOptions() {
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;  // high, so only the deadline (or the close) can flush
    opt.timeout = kDeadlineTimeout;
    return opt;
}

void ExpectNullTimeoutReport(const BatchSink::Delivery& d, int64_t dropped) {
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kTimeout);
    EXPECT_EQ(d.batch, nullptr);
    EXPECT_EQ(d.status.rows_dropped, dropped);
    EXPECT_TRUE(d.attachments.empty());
}

// Waits until the deliveries so far report exactly `total` lost rows in all, each as a null
// kTimeout report. A stalled window may split its rows across several reports, so the number of
// deliveries is not assumed. Returns the number of deliveries on success, 0 on failure (reported).
size_t WaitForNullTimeoutReportsTotalling(BatchSink& sink, int64_t total) {
    std::unique_lock<std::mutex> lk(sink.mu);
    const auto dropped = [&] {
        int64_t n = 0;
        for (const auto& d : sink.deliveries) n += d.status.rows_dropped;
        return n;
    };
    if (!sink.cv.wait_for(lk, std::chrono::seconds(2), [&] { return dropped() >= total; })) {
        ADD_FAILURE() << "reports add up to " << dropped() << " lost rows, expected " << total
                      << ": the subscription stayed silent while open";
        return 0;
    }
    EXPECT_EQ(dropped(), total);
    for (const auto& d : sink.deliveries) {
        ExpectNullTimeoutReport(d, d.status.rows_dropped);
        EXPECT_GT(d.status.rows_dropped, 0);  // no report for an idle window (L6)
    }
    return sink.deliveries.size();
}

// Checks the deliveries from `first` on, after the topic's schema became usable and three
// well-formed rows (x = 1, 2, 3; the attachment on x = 2) were published: every delivery is a
// non-null batch with no drops and aligned attachments; within one delivery the rows are in
// order; across deliveries each of x = 1, 2, 3 appears exactly once with its attachment on x = 2.
// Order across deliveries is deliberately not asserted: while L12 is open a stalled
// kTimeout/kClosing split can be recorded in either order, and that order is rung 3's subject.
// Caller holds sink.mu.
void ExpectLateSchemaRowsAssembled(const std::vector<BatchSink::Delivery>& ds, size_t first) {
    ASSERT_GT(ds.size(), first);
    std::vector<std::pair<int32_t, bool>> seen;  // (x, has attachment)
    for (size_t i = first; i < ds.size(); ++i) {
        const auto& d = ds[i];
        ASSERT_NE(d.batch, nullptr) << "delivery " << i;
        EXPECT_EQ(d.status.rows_dropped, 0) << "delivery " << i;
        ASSERT_EQ(d.attachments.size(), static_cast<size_t>(d.batch->num_rows())) << "delivery " << i;
        const auto x = std::static_pointer_cast<arrow::Int32Array>(d.batch->column(0));
        for (int64_t r = 0; r < d.batch->num_rows(); ++r) {
            if (r > 0) EXPECT_LT(x->Value(r - 1), x->Value(r)) << "delivery " << i;
            seen.emplace_back(x->Value(r), d.attachments[r].Find("orphan") != nullptr);
        }
    }
    std::sort(seen.begin(), seen.end());
    const std::vector<std::pair<int32_t, bool>> expected = {{1, false}, {2, true}, {3, false}};
    EXPECT_EQ(seen, expected);
}

}  // namespace

// Forcing (mock): the topic is not declared when the rows arrive, so the Mock hands every row a
// null schema. Two windows of lost rows each get a null kTimeout report at their own deadline
// while the subscription stays open; an idle stretch reports nothing; a schema that becomes
// usable later gives normal batches; and rows received plus rows reported lost equal rows sent.
TEST(SubscriberArrowBatchTest, NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles) {
    BatchSink sink;  // declared first: it must outlive the subscription, even on a failed ASSERT
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    auto result = sub.Subscribe(kTopic, sink.callback(), DeadlineBatchOptions());

    // W1: three lost rows, one with an attachment.
    PublishSchemaLessRows(pub);
    const size_t after_w1 = WaitForNullTimeoutReportsTotalling(sink, kSchemaLessRows);
    ASSERT_GT(after_w1, 0u);

    // Idle: nothing happens for three timeouts, so nothing is reported (L6).
    std::this_thread::sleep_for(3 * kDeadlineTimeout);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        EXPECT_EQ(sink.deliveries.size(), after_w1);
    }

    // W2: one more lost row, reported at its own deadline.
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });
    const size_t after_w2 = WaitForNullTimeoutReportsTotalling(sink, kSchemaLessRows + 1);
    ASSERT_GT(after_w2, after_w1);

    // Late schema: the topic is declared and good rows arrive; a batch can be built again (L13).
    pub.CreateTopic(kTopic, TestSchema());
    PublishWellFormedRows(pub);
    sub.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    ExpectLateSchemaRowsAssembled(sink.deliveries, after_w2);
    int64_t accounted = 0;  // rows received + rows reported lost
    for (const auto& d : sink.deliveries) {
        accounted += (d.batch ? d.batch->num_rows() : 0) + d.status.rows_dropped;
    }
    EXPECT_EQ(accounted, kSchemaLessRows + 1 + kWellFormedRows);
}

// Forcing (in-process variant). The in-process provider (schema_carriage=as_declared) latches a
// subscription's schema once, at Subscribe, so a subscription made before the declaration has no
// schema for life. This pins L1/L6/L7 for "no usable schema" at each deadline; it does not pin
// that a late schema is unusable, which is the provider's rule and not Fletcher's.
TEST(SubscriberArrowBatchTest, SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline) {
    BatchSink sink;
    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    SubscriberArrow sub(provider);

    auto result = sub.Subscribe(kTopic, sink.callback(), DeadlineBatchOptions());

    pub.CreateTopic(kTopic, TestSchema());
    PublishWellFormedRows(pub);  // W1
    const size_t after_w1 = WaitForNullTimeoutReportsTotalling(sink, kWellFormedRows);
    ASSERT_GT(after_w1, 0u);

    pub.Publish(kTopic, MakeRow(4, "d"));  // W2
    const size_t after_w2 = WaitForNullTimeoutReportsTotalling(sink, kWellFormedRows + 1);
    ASSERT_GT(after_w2, after_w1);

    // Both windows were reported; nothing is left, so the close delivers exactly one empty closing,
    // null (no decoder: no usable schema ever arrived), last (ruling 2026-11-14, L7).
    sub.Unsubscribe(result.subscription_id);
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), after_w2 + 1);
    const auto& closing = sink.deliveries.back();
    EXPECT_EQ(closing.status.reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(closing.batch, nullptr);
    EXPECT_EQ(closing.status.rows_dropped, 0);
    EXPECT_TRUE(closing.attachments.empty());
}

namespace {

enum class AssemblyState { kAwaitingSchema, kUndecodableSchema, kUnimportableSchema, kAssembling };
enum class Ending { kDeadline, kClose };

// A struct whose child has a format string Arrow does not know: Publisher::CreateTopic accepts it
// (the declaration's encoder never throws) and a provider hands it on, but ImportArrowSchema throws.
OwnedSchema UnimportableSchema() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "x");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    ArrowSchemaSetFormat(schema->children[0], "zz");
    return schema;
}

// One cell of the guard: a fresh subscription in `state`, one malformed row, ended by `ending`.
// The unimportable cell also proves the failed import is final (L8): after the first row (and its
// report, at the deadline ending) the topic is declared with a good schema and a well-formed row is
// sent, and the subscription still drops it instead of reviving.
void RunAssemblyCell(AssemblyState state, Ending ending) {
    BatchSink sink;
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    if (state == AssemblyState::kUndecodableSchema) {
        // BatchDecoder rejects a dictionary below the top level.
        auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        pub.CreateTopic(kTopic, arrow::schema({arrow::field("tags", arrow::list(dict_type))}));
    } else if (state == AssemblyState::kUnimportableSchema) {
        // The cell must not pass vacuously: the schema really is one the import rejects.
        ASSERT_ANY_THROW(
            static_cast<void>(ImportArrowSchema(MakeSharedSchema(UnimportableSchema()))));
        Publisher(mock).CreateTopic(kTopic, UnimportableSchema());
    } else if (state == AssemblyState::kAssembling) {
        pub.CreateTopic(kTopic, TestSchema());
    }  // kAwaitingSchema: the topic is never declared

    auto result = sub.Subscribe(kTopic, sink.callback(),
                                ending == Ending::kDeadline ? DeadlineBatchOptions()
                                                            : NoTriggerBatchOptions());
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });

    if (ending == Ending::kDeadline) {
        ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)))
            << "no report at the deadline";
    }

    // Rows lost in each expected delivery: one, or (unimportable) a second row that a revived
    // subscription would have decoded.
    std::vector<int64_t> expected_dropped = {1};
    if (state == AssemblyState::kUnimportableSchema) {
        pub.CreateTopic(kTopic, TestSchema());  // the topic now carries a good schema
        pub.Publish(kTopic, MakeRow(1, "a"));
        if (ending == Ending::kDeadline) {
            ASSERT_TRUE(sink.WaitFor(2, std::chrono::seconds(2)))
                << "no report at the second deadline";
            expected_dropped = {1, 1};
        } else {
            expected_dropped = {2};
        }
    }
    sub.Unsubscribe(result.subscription_id);

    // At the deadline ending every loss was reported before the close, so the close adds exactly
    // one empty closing delivery, last (ruling 2026-11-14): zero rows lost, of the cell's shape.
    std::vector<BatchStatus::Reason> expected_reason(
        expected_dropped.size(), ending == Ending::kDeadline ? BatchStatus::Reason::kTimeout
                                                             : BatchStatus::Reason::kClosing);
    if (ending == Ending::kDeadline) {
        expected_dropped.push_back(0);
        expected_reason.push_back(BatchStatus::Reason::kClosing);
    }
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), expected_dropped.size());  // exactly these: not none, not more
    for (size_t i = 0; i < expected_dropped.size(); ++i) {
        const auto& d = sink.deliveries[i];
        EXPECT_EQ(d.status.reason, expected_reason[i]) << "delivery " << i;
        EXPECT_EQ(d.status.rows_dropped, expected_dropped[i]) << "delivery " << i;
        EXPECT_TRUE(d.attachments.empty()) << "delivery " << i;
        if (state == AssemblyState::kAssembling) {
            ASSERT_NE(d.batch, nullptr);  // a batch could be built: non-null, zero rows (L7)
            EXPECT_EQ(d.batch->num_rows(), 0);
        } else {
            EXPECT_EQ(d.batch, nullptr) << "delivery " << i;  // no batch could be built (L7)
        }
    }
}

}  // namespace

// Guard (green once D7F-2 lands): every state the assembly can be in reports its loss at either
// ending. Forbids a state that suppresses reporting, which is how the schema-less deadline was
// silent while the other cells were not. The unimportable-schema cells also pin L8: a failed
// import is decided once and is permanent.
TEST(SubscriberArrowBatchTest, EveryAssemblyStateReportsItsDropsAtEitherEnding) {
    struct StateName {
        AssemblyState state;
        const char* name;
    };
    for (const auto& s : {StateName{AssemblyState::kAwaitingSchema, "awaiting a schema"},
                          StateName{AssemblyState::kUndecodableSchema, "undecodable schema"},
                          StateName{AssemblyState::kUnimportableSchema, "unimportable schema"},
                          StateName{AssemblyState::kAssembling, "assembling"}}) {
        for (const Ending ending : {Ending::kDeadline, Ending::kClose}) {
            SCOPED_TRACE(std::string(s.name) +
                         (ending == Ending::kDeadline ? " x deadline" : " x close"));
            RunAssemblyCell(s.state, ending);
        }
    }
}

// Leg: a schema that arrives in the middle of a window gives one normal batch carrying the rows
// lost before it (L2, L3, L7): the batch's shape is decided when the window closes.
TEST(SubscriberArrowBatchTest, SchemaArrivingMidWindowGivesOneBatchCarryingEarlierDrops) {
    BatchSink sink;
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    auto result = sub.Subscribe(kTopic, sink.callback(), NoTriggerBatchOptions());

    PublishSchemaLessRows(pub);
    pub.CreateTopic(kTopic, TestSchema());
    pub.Publish(kTopic, MakeRow(1, "a"));
    pub.Publish(kTopic, MakeRow(2, "b"), OrphanAttachments());
    sub.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    const auto& d = sink.deliveries[0];
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
    ASSERT_NE(d.batch, nullptr);
    EXPECT_EQ(d.batch->num_rows(), 2);
    EXPECT_EQ(d.status.rows_dropped, kSchemaLessRows);
    ASSERT_EQ(d.attachments.size(), 2u);
    EXPECT_EQ(d.attachments[0].Find("orphan"), nullptr);
    EXPECT_NE(d.attachments[1].Find("orphan"), nullptr);
}

// ---------------------------------------------------------------------------
// Dictionary columns — transferred as values, re-folded into a DictionaryArray
// ---------------------------------------------------------------------------

TEST(SubscriberArrowBatchTest, DictionaryColumnRefoldedToDictionaryArray) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    pub.CreateTopic(kTopic, arrow::schema({arrow::field("category", dict_type, true)}));

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 3;
    opt.timeout = std::chrono::minutes(10);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    // Published as plain values; subscriber re-folds into a dictionary.
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("red")});
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("blue")});
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("red")});

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    auto batch = sink.deliveries[0].batch;
    ASSERT_NE(batch, nullptr);
    ASSERT_EQ(batch->num_columns(), 1);
    auto col = batch->column(0);
    ASSERT_EQ(col->type_id(), arrow::Type::DICTIONARY);
    EXPECT_TRUE(col->type()->Equals(*dict_type));

    auto dict_col = std::static_pointer_cast<arrow::DictionaryArray>(col);
    EXPECT_EQ(dict_col->length(), 3);
    EXPECT_EQ(dict_col->dictionary()->length(), 2);  // "red", "blue"

    auto idx = std::static_pointer_cast<arrow::Int32Array>(dict_col->indices());
    EXPECT_EQ(idx->Value(0), idx->Value(2));  // both "red" -> same index
    EXPECT_NE(idx->Value(0), idx->Value(1));  // "red" vs "blue"

    auto values = std::static_pointer_cast<arrow::StringArray>(dict_col->dictionary());
    EXPECT_EQ(values->GetString(idx->Value(0)), "red");
    EXPECT_EQ(values->GetString(idx->Value(1)), "blue");
}

TEST(SubscriberArrowBatchTest, DictionaryColumnPreservesNulls) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    pub.CreateTopic(kTopic, arrow::schema({arrow::field("category", dict_type, true)}));

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 3;
    opt.timeout = std::chrono::minutes(10);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("x")});
    pub.Publish(kTopic, {arrow::MakeNullScalar(arrow::utf8())});
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("x")});

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    auto col = sink.deliveries[0].batch->column(0);
    ASSERT_EQ(col->type_id(), arrow::Type::DICTIONARY);
    EXPECT_EQ(col->length(), 3);
    EXPECT_TRUE(col->IsValid(0));
    EXPECT_FALSE(col->IsValid(1));  // null preserved through re-folding
    EXPECT_TRUE(col->IsValid(2));

    auto dict_col = std::static_pointer_cast<arrow::DictionaryArray>(col);
    EXPECT_EQ(dict_col->dictionary()->length(), 1);  // only "x"
}

// ---------------------------------------------------------------------------
// BatchDecoder-backed batching — corrupt rows, capacity, reuse
// ---------------------------------------------------------------------------

TEST(SubscriberArrowBatchTest, CorruptRowIsCountedDroppedAndBatchStaysAligned) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto schema = arrow::schema({
        arrow::field("tags", arrow::list(arrow::utf8())),
        arrow::field("x", arrow::int32()),
    });
    pub.CreateTopic(kTopic, schema);

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 2;  // A and B; the corrupt row in between never counts toward the limit
    opt.timeout = std::chrono::minutes(10);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    arrow::StringBuilder sb_a;
    ASSERT_TRUE(sb_a.AppendValues({"a", "b"}).ok());
    ArrowRow row_a = {std::make_shared<arrow::ListScalar>(sb_a.Finish().ValueOrDie()),
                      std::make_shared<arrow::Int32Scalar>(1)};
    arrow::StringBuilder sb_b;
    ASSERT_TRUE(sb_b.AppendValues({"c"}).ok());
    ArrowRow row_b = {std::make_shared<arrow::ListScalar>(sb_b.Finish().ValueOrDie()),
                      std::make_shared<arrow::Int32Scalar>(2)};

    Blob blob_a{std::vector<uint8_t>{0xAA}};
    Blob blob_b{std::vector<uint8_t>{0xBB}};
    Attachments att_a;
    att_a.Set("blob", blob_a);
    Attachments att_b;
    att_b.Set("blob", blob_b);

    pub.Publish(kTopic, row_a, att_a);

    // A corrupt row: row A's own encoding, truncated by 3 bytes so the fixed 4-byte
    // int32 field underruns on read — BatchDecoder throws, nothing is appended.
    Codec codec(schema);
    EncodedRow encoded_a = codec.EncodeRow(row_a);
    ASSERT_GT(encoded_a.size(), 3u);
    EncodedRow truncated(encoded_a.begin(), encoded_a.end() - 3);
    pub.PublishDirect(kTopic,
                      [&](WriteBuffer& buf) { buf.Append(truncated.data(), truncated.size()); });

    pub.Publish(kTopic, row_b, att_b);  // reaches max_rows: the window closes

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    const auto& d = sink.deliveries[0];
    EXPECT_EQ(d.num_rows, 2);
    EXPECT_EQ(d.status.rows_dropped, 1);
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kRowLimit);
    ASSERT_EQ(d.attachments.size(), 2u);
    const Blob* got_a = d.attachments[0].Find("blob");
    ASSERT_NE(got_a, nullptr);
    EXPECT_EQ(got_a->data(), blob_a.data());
    EXPECT_EQ(got_a->size(), blob_a.size());
    const Blob* got_b = d.attachments[1].Find("blob");
    ASSERT_NE(got_b, nullptr);
    EXPECT_EQ(got_b->data(), blob_b.data());
    EXPECT_EQ(got_b->size(), blob_b.size());

    ASSERT_NE(d.batch, nullptr);
    auto x_col = std::static_pointer_cast<arrow::Int32Array>(d.batch->column(1));
    EXPECT_EQ(x_col->Value(0), 1);
    EXPECT_EQ(x_col->Value(1), 2);
    auto tags_col = std::static_pointer_cast<arrow::ListArray>(d.batch->column(0));
    EXPECT_EQ(tags_col->value_length(0), 2);
    EXPECT_EQ(tags_col->value_length(1), 1);
}

TEST(SubscriberArrowBatchTest, FixedSizeListWithNamedItemArrivesNonNull) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto fsl_type = arrow::fixed_size_list(arrow::field("v", arrow::float32(), false), 3);
    auto schema = arrow::schema({arrow::field("vec", fsl_type)});
    pub.CreateTopic(kTopic, schema);

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 1;
    opt.timeout = std::chrono::minutes(10);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    arrow::FloatBuilder fb;
    ASSERT_TRUE(fb.AppendValues({1.0f, 2.0f, 3.0f}).ok());
    auto fsl_scalar =
        std::make_shared<arrow::FixedSizeListScalar>(fb.Finish().ValueOrDie(), fsl_type);
    pub.Publish(kTopic, {fsl_scalar});

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    auto batch = sink.deliveries[0].batch;
    ASSERT_NE(batch, nullptr);
    ASSERT_EQ(batch->num_rows(), 1);
    auto col = batch->column(0);
    ASSERT_TRUE(col->IsValid(0));  // this was silently nulled before the underlying fix
    auto fsl_col = std::static_pointer_cast<arrow::FixedSizeListArray>(col);
    auto values = std::static_pointer_cast<arrow::FloatArray>(fsl_col->values());
    EXPECT_EQ(values->Value(0), 1.0f);
    EXPECT_EQ(values->Value(1), 2.0f);
    EXPECT_EQ(values->Value(2), 3.0f);
}

TEST(SubscriberArrowBatchTest, NestedDictionarySchemaReportsEveryRowDropped) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({arrow::field("tags", arrow::list(dict_type))});
    pub.CreateTopic(kTopic, schema);

    BatchSink sink;
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::minutes(10);
    auto result = sub.Subscribe(kTopic, sink.callback(), opt);

    // BatchDecoder rejects this schema (a dictionary below the top level), so the row
    // content never matters -- every row is dropped without an attempt to decode it.
    for (int i = 0; i < 3; ++i) {
        pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });
    }
    sub.Unsubscribe(result.subscription_id);  // force the closing flush

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    const auto& d = sink.deliveries[0];
    EXPECT_EQ(d.batch, nullptr);
    EXPECT_EQ(d.status.rows_dropped, 3);
    EXPECT_TRUE(d.attachments.empty());
}

TEST(SubscriberArrowBatchTest, FinishFailureIsReportedNotFatal) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int8(), arrow::utf8());
    pub.CreateTopic(kTopic, arrow::schema({arrow::field("d", dict_type, true)}));

    BatchSink sink;
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 200;
    opt.timeout = std::chrono::minutes(10);
    auto result = sub.Subscribe(kTopic, sink.callback(), opt);

    // 200 distinct values overflow an int8 dictionary index (128 representable
    // values) when BatchDecoder::Finish() re-folds the column and casts it to
    // dictionary<int8>. That failure is an internal invariant, not a
    // malformed row, and must not terminate the process.
    for (int i = 0; i < 200; ++i) {
        pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("v" + std::to_string(i))});
    }

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        ASSERT_EQ(sink.deliveries.size(), 1u);
        EXPECT_EQ(sink.deliveries[0].batch, nullptr);
        EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 200);
    }

    // The decoder was reset, not left broken: a following window with few
    // distinct values delivers a normal batch (closing flush forces it out
    // rather than waiting on the 10-minute timeout).
    for (const auto& v : {"a", "b", "c"}) {
        pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>(v)});
    }
    sub.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 2u);
    EXPECT_EQ(sink.deliveries[1].num_rows, 3);
    EXPECT_EQ(sink.deliveries[1].status.rows_dropped, 0);
}

// Unsubscribe called from INSIDE a kRowLimit batch callback (L9, L11). max_rows = 1, so the first
// row is a full window of its own and nothing is pending when the callback unsubscribes: the close
// still ends with exactly one closing delivery, empty (zero rows: the subscription has a decoder),
// after the callback returns, and nothing may follow (ruling 2026-11-14). The callback signals once
// its Unsubscribe has returned; only then are rows 2-3 published, and the deliveries are read after
// ~SubscriberArrow, which waits for every delivery the subscription still owes.
TEST(SubscriberArrowBatchTest, UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    std::atomic<int> delivery_count{0};
    std::atomic<uint64_t> sub_id{0};
    std::promise<void> unsubscribed;
    std::future<void> unsubscribed_future = unsubscribed.get_future();
    std::mutex got_mu;
    std::vector<std::pair<BatchStatus, int64_t>> got;  // (status, rows; -1 for a null batch)
    auto sub = std::make_unique<SubscriberArrow>(mock);
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 1;  // every published row is a full window of its own
    opt.timeout = std::chrono::minutes(10);
    auto result = sub->Subscribe(
        kTopic,
        [&](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments>,
            BatchStatus status) {
            {
                std::lock_guard<std::mutex> lk(got_mu);
                got.emplace_back(status, batch ? batch->num_rows() : -1);
            }
            if (++delivery_count != 1) return;  // recorded; judged below
            sub->Unsubscribe(sub_id.load());
            unsubscribed.set_value();
        },
        opt);
    sub_id = result.subscription_id;

    pub.Publish(kTopic, MakeRow(1, "a"));  // a full window -> the callback above
    ASSERT_EQ(unsubscribed_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // Rows after the close reach no one, and the process has not crashed getting here.
    pub.Publish(kTopic, MakeRow(2, "b"));
    pub.Publish(kTopic, MakeRow(3, "c"));
    sub.reset();
    std::lock_guard<std::mutex> lk(got_mu);
    ASSERT_EQ(got.size(), 2u) << "the full window, then exactly one closing delivery";
    EXPECT_EQ(got[0].first.reason, BatchStatus::Reason::kRowLimit);
    EXPECT_EQ(got[0].second, 1);
    EXPECT_EQ(got[1].first.reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(got[1].first.rows_dropped, 0);
    EXPECT_EQ(got[1].second, 0) << "an empty closing is a zero-row batch, never null, here (L7)";
}

// Pins L11 (RecordBatchBatcher::Start's fix): the callback runs on the subscription's own batch
// thread, so an Unsubscribe from inside it makes that same thread take Stop()'s self-detach
// branch. Before the fix the thread's lambda held only a raw `this`, so the shared_ptr
// Unsubscribe drops there was the last owner and destroyed the batcher while the thread was still
// running inside it (a use-after-free that may or may not crash under a given allocator). With the
// fix the thread owns a shared_ptr of its own, kept alive until its loop returns, so the object
// outlives the thread's use of it.
TEST(SubscriberArrowBatchTest, UnsubscribeFromInsideATimeoutFlushIsSafe) {
    // Declared first: what the callback captures outlives the subscriber, whose last delivery (the
    // closing one) comes after the in-callback Unsubscribe returns.
    std::mutex id_mu;
    uint64_t sub_id = 0;
    std::vector<std::pair<BatchStatus, int64_t>> got;  // guarded by id_mu; rows -1 when null
    std::promise<void> delivered;
    std::future<void> delivered_future = delivered.get_future();
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    auto sub = std::make_unique<SubscriberArrow>(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100;  // never reached; only the timeout triggers the flush
    opt.timeout = std::chrono::milliseconds(20);
    auto result = sub->Subscribe(
        kTopic,
        [&](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments>,
            BatchStatus status) {
            // The one window was taken by this kTimeout delivery, so nothing is pending when the
            // Unsubscribe below runs: it still owes exactly one closing delivery, empty, after
            // this callback returns, never nested in that Unsubscribe (L9, L12; ruling 2026-11-14).
            uint64_t id;
            size_t index;
            {
                std::lock_guard<std::mutex> lk(id_mu);
                index = got.size();
                got.emplace_back(status, batch ? batch->num_rows() : -1);
                id = sub_id;
            }
            if (index != 0) return;
            sub->Unsubscribe(id);  // runs on the batch thread -- the path the fix protects
            {
                std::lock_guard<std::mutex> lk(id_mu);
                EXPECT_EQ(got.size(), 1u) << "a batch was delivered inside the Unsubscribe";
            }
            delivered.set_value();
        },
        opt);
    {
        std::lock_guard<std::mutex> lk(id_mu);
        sub_id = result.subscription_id;  // visible to the callback before the row is published
    }

    pub.Publish(kTopic, MakeRow(1, "a"));

    ASSERT_EQ(delivered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    // Without the fix the batch thread would already have used freed memory before this point.
    sub.reset();  // waits for the closing delivery
    std::lock_guard<std::mutex> lk(id_mu);
    ASSERT_EQ(got.size(), 2u) << "the kTimeout window, then exactly one closing delivery";
    EXPECT_EQ(got[0].first.reason, BatchStatus::Reason::kTimeout);
    EXPECT_EQ(got[1].first.reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(got[1].first.rows_dropped, 0);
    EXPECT_EQ(got[1].second, 0) << "an empty closing is a zero-row batch, never null, here (L7)";
}

TEST(SubscriberArrowBatchTest, BatchesAreValidArrow) {
    BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({
        arrow::field("x", arrow::int32()),
        arrow::field("name", arrow::utf8()),
        arrow::field("category", dict_type, true),
        arrow::field("tags", arrow::list(arrow::utf8())),
    });
    pub.CreateTopic(kTopic, schema);

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 5;
    opt.timeout = std::chrono::minutes(10);
    static_cast<void>(sub.Subscribe(kTopic, sink.callback(), opt));

    for (int i = 0; i < 5; ++i) {
        arrow::StringBuilder sb;
        ASSERT_TRUE(sb.AppendValues({"t" + std::to_string(i)}).ok());
        ArrowRow row = {
            std::make_shared<arrow::Int32Scalar>(i),
            std::make_shared<arrow::StringScalar>("n" + std::to_string(i)),
            std::make_shared<arrow::StringScalar>(i % 2 == 0 ? "red" : "blue"),
            std::make_shared<arrow::ListScalar>(sb.Finish().ValueOrDie()),
        };
        pub.Publish(kTopic, row);
    }

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    auto batch = sink.deliveries[0].batch;
    ASSERT_NE(batch, nullptr);
    EXPECT_EQ(batch->num_rows(), 5);
    EXPECT_TRUE(batch->ValidateFull().ok());
}

TEST(SubscriberArrowBatchTest, ReuseAcrossWindows) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 2;
    opt.timeout = std::chrono::minutes(10);
    auto result = sub.Subscribe(kTopic, sink.callback(), opt);

    for (int i = 0; i < 5; ++i) pub.Publish(kTopic, MakeRow(i, "n" + std::to_string(i)));
    sub.Unsubscribe(result.subscription_id);  // closing flush delivers the trailing partial batch

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 3u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 2);
    EXPECT_EQ(sink.deliveries[1].num_rows, 2);
    EXPECT_EQ(sink.deliveries[2].num_rows, 1);
    for (const auto& d : sink.deliveries) EXPECT_EQ(d.status.rows_dropped, 0);

    int32_t expected = 0;
    for (const auto& d : sink.deliveries) {
        ASSERT_NE(d.batch, nullptr);
        auto col = std::static_pointer_cast<arrow::Int32Array>(d.batch->column(0));
        for (int64_t r = 0; r < col->length(); ++r) {
            EXPECT_EQ(col->Value(r), expected++);
        }
    }
}

// ---------------------------------------------------------------------------
// D7F-3 -- one batch thread, in order, nothing lost at close (D10, L1, L4, L9, L11, L12)
//
// Every batch callback runs on the subscription's own batch thread: never inside Publish, never on
// the thread that unsubscribes, one batch at a time, in window order. Closing from outside waits
// for every queued window and then the kClosing batch; closing from inside a callback (or from a
// thread delivering rows on the subscription's own transport) returns at once and those batches
// follow on the batch thread. Observed from the user callback only: an in-flight count, a
// per-thread nesting depth, and (reason, first x, rows, drops, thread) per delivery. Every wait is
// bounded, so a red run fails rather than hangs. "Then nothing" is read after ~SubscriberArrow.
// ---------------------------------------------------------------------------

namespace {

// How long a held callback waits for its release, and how long the test thread waits for any
// event. On a correct tree each wait ends within the 200 ms batch timer or kLetItWait. On a broken
// one a test row runs at most about (3 x kAwait + kHold) per case, 5 cases at most, so even a red
// row ends well inside the suite's 30 s ctest TIMEOUT. The seeded guards wait up to kSeedAwait
// for a seed's events (a seed takes ~0.25 s on a coarse timer) and stop at the first failed seed,
// so a red row costs one seed's waits, not sixteen.
constexpr auto kHold = std::chrono::milliseconds(1000);
constexpr auto kAwait = std::chrono::milliseconds(1000);
constexpr auto kSeedAwait = std::chrono::milliseconds(2000);
// How long a helper lets a closing call sit before releasing the callback it waits behind.
constexpr auto kLetItWait = std::chrono::milliseconds(100);

const std::vector<std::string> kOtherTopic = {"test", "other"};

class Latch {
   public:
    void Open() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            open_ = true;
        }
        cv_.notify_all();
    }
    bool WaitFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return open_; });
    }
    bool IsOpen() {
        std::lock_guard<std::mutex> lk(mu_);
        return open_;
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
};

thread_local int t_batch_callback_depth = 0;

void RaiseTo(std::atomic<int>& peak, int value) {
    int seen = peak.load();
    while (value > seen && !peak.compare_exchange_weak(seen, value)) {
    }
}

// The user callback's view of every delivery. `hook` runs inside the delivery, after it is
// recorded; it is set before subscribing and never changed.
struct DeliveryLog {
    struct Rec {
        BatchStatus::Reason reason;
        int64_t num_rows;  // -1 for a null batch
        int32_t first_x;   // -1 when the batch has no row
        int32_t last_x;    // -1 when the batch has no row
        int64_t dropped;
        std::thread::id thread;
        std::shared_ptr<arrow::Schema> schema;  // the batch's; null for a null batch
    };

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Rec> recs;
    size_t returned = 0;  // guarded by mu
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    std::atomic<int> max_depth{0};
    std::function<void(size_t index, const Rec& rec)> hook;

    SubscriberArrow::RecordBatchCallback callback() {
        return [this](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments>,
                      BatchStatus status) { OnDelivery(batch, status); };
    }

    void OnDelivery(const std::shared_ptr<arrow::RecordBatch>& batch, BatchStatus status) {
        RaiseTo(max_in_flight, ++in_flight);
        RaiseTo(max_depth, ++t_batch_callback_depth);
        Rec rec{status.reason,
                batch ? batch->num_rows() : -1,
                -1,
                -1,
                status.rows_dropped,
                std::this_thread::get_id(),
                batch ? batch->schema() : nullptr};
        if (batch && batch->num_rows() > 0 && batch->num_columns() > 0 &&
            batch->column(0)->type_id() == arrow::Type::INT32) {
            const auto x = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
            rec.first_x = x->Value(0);
            rec.last_x = x->Value(x->length() - 1);
        }
        size_t index;
        {
            std::lock_guard<std::mutex> lk(mu);
            index = recs.size();
            recs.push_back(rec);
        }
        cv.notify_all();
        if (hook) hook(index, rec);
        --t_batch_callback_depth;
        --in_flight;
        {
            std::lock_guard<std::mutex> lk(mu);
            ++returned;
        }
        cv.notify_all();
    }

    size_t Count() {
        std::lock_guard<std::mutex> lk(mu);
        return recs.size();
    }
    bool WaitFor(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return recs.size() >= n; });
    }
    // Every callback recorded so far has returned, so none of this test's state is still in use
    // on a thread the test does not own.
    bool WaitAllReturned(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return returned == recs.size(); });
    }
    std::vector<Rec> Snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return recs;
    }
};

const char* ReasonName(BatchStatus::Reason r) {
    switch (r) {
        case BatchStatus::Reason::kRowLimit:
            return "kRowLimit";
        case BatchStatus::Reason::kTimeout:
            return "kTimeout";
        case BatchStatus::Reason::kClosing:
            return "kClosing";
    }
    return "?";
}

struct Expected {
    BatchStatus::Reason reason;
    int32_t first_x;
    int64_t num_rows;
};

// The deliveries are exactly `expected`, in that order, every one on the same thread, which is
// none of `foreign` (the publishing, unsubscribing and destroying threads), and never two at once
// or one inside another.
void ExpectDeliveredInOrderOnOneBatchThread(DeliveryLog& log, const std::vector<Expected>& expected,
                                            const std::vector<std::thread::id>& foreign) {
    const auto recs = log.Snapshot();
    EXPECT_LE(log.max_in_flight.load(), 1) << "two batch callbacks ran at once (L12)";
    EXPECT_LE(log.max_depth.load(), 1) << "a batch was delivered inside a batch callback (L12)";
    EXPECT_EQ(recs.size(), expected.size()) << "number of deliveries";
    for (size_t i = 0; i < recs.size(); ++i) {
        SCOPED_TRACE("delivery " + std::to_string(i) + " (" + ReasonName(recs[i].reason) +
                     ", first x " + std::to_string(recs[i].first_x) + ")");
        if (i < expected.size()) {
            EXPECT_EQ(recs[i].reason, expected[i].reason);
            EXPECT_EQ(recs[i].first_x, expected[i].first_x);
            EXPECT_EQ(recs[i].num_rows, expected[i].num_rows);
        }
        EXPECT_EQ(recs[i].dropped, 0);
        EXPECT_EQ(recs[i].thread, recs[0].thread) << "not the thread of the first delivery (L12)";
        for (const auto& id : foreign) {
            EXPECT_NE(recs[i].thread, id) << "delivered on a publishing or closing thread (D10)";
        }
    }
}

SubscriberArrow::BatchOptions RowLimitOptions(int64_t max_rows) {
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = max_rows;
    opt.timeout = std::chrono::minutes(10);  // only the count (or the close) closes a window
    return opt;
}

}  // namespace

// Rung 3 (D10, L12). max_rows = 2; a second thread publishes 6 rows. The first callback holds until
// the publisher reports that every Publish returned, which a Publish that ran or waited on a
// callback can never report.
TEST(SubscriberArrowBatchTest, EveryCallbackRunsOnTheBatchThreadInWindowOrder) {
    DeliveryLog log;
    Latch entered, all_published;
    std::atomic<bool> first_released_by_publisher{false};
    log.hook = [&](size_t index, const DeliveryLog::Rec&) {
        if (index != 0) return;
        entered.Open();
        first_released_by_publisher = all_published.WaitFor(kHold);
    };

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, TestSchema());
    auto sub = std::make_unique<SubscriberArrow>(provider);
    static_cast<void>(sub->Subscribe(kTopic, log.callback(), RowLimitOptions(2)));

    std::thread::id publisher_id;
    std::atomic<bool> entered_before_rest{false};
    std::thread publisher([&] {
        publisher_id = std::this_thread::get_id();
        pub.Publish(kTopic, MakeRow(0, "n"));
        pub.Publish(kTopic, MakeRow(1, "n"));  // the first full window
        entered_before_rest = entered.WaitFor(kAwait);
        for (int i = 2; i < 6; ++i) pub.Publish(kTopic, MakeRow(i, "n"));
        all_published.Open();
    });
    publisher.join();

    EXPECT_TRUE(log.WaitFor(3, kAwait));
    EXPECT_TRUE(log.WaitAllReturned(kAwait));
    sub.reset();

    EXPECT_TRUE(entered_before_rest.load());
    EXPECT_TRUE(first_released_by_publisher.load())
        << "the publisher could not finish publishing while the first callback was held: "
           "Publish ran or waited on a batch callback (D10)";
    // sub.reset() closed an idle window: exactly one empty (zero-row) closing ends it (2026-11-14).
    ExpectDeliveredInOrderOnOneBatchThread(log,
                                           {{BatchStatus::Reason::kRowLimit, 0, 2},
                                            {BatchStatus::Reason::kRowLimit, 2, 2},
                                            {BatchStatus::Reason::kRowLimit, 4, 2},
                                            {BatchStatus::Reason::kClosing, -1, 0}},
                                           {publisher_id, std::this_thread::get_id()});
}

namespace {

enum class OutsideClosing { kUnsubscribe, kDestructor, kRacingUnsubscribe };

// Rung 4 (a), (b), (d). max_rows = 2. The first kRowLimit callback is held while a publisher queues
// two more full windows (x = 2..5) and leaves one row pending (x = 6). The test thread then closes
// from outside, and a helper releases the held callback kLetItWait later. The closing call returns
// only after kRowLimit x3 and then kClosing(1), all on the batch thread; nothing comes after.
void RunClosingFromAnotherThread(OutsideClosing closing) {
    DeliveryLog log;
    Latch entered, published, release;
    std::atomic<bool> released_by_helper{false};
    std::atomic<bool> first_returned{false};
    log.hook = [&](size_t index, const DeliveryLog::Rec&) {
        if (index != 0) return;
        entered.Open();
        released_by_helper = release.WaitFor(kHold);
        first_returned = true;
    };

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, TestSchema());
    auto sub = std::make_unique<SubscriberArrow>(provider);
    const uint64_t id = sub->Subscribe(kTopic, log.callback(), RowLimitOptions(2)).subscription_id;

    std::thread::id publisher_id;
    std::atomic<bool> published_while_held{false};
    std::thread publisher([&] {
        publisher_id = std::this_thread::get_id();
        pub.Publish(kTopic, MakeRow(0, "n"));
        pub.Publish(kTopic, MakeRow(1, "n"));
        static_cast<void>(entered.WaitFor(kAwait));
        for (int i = 2; i < 7; ++i) pub.Publish(kTopic, MakeRow(i, "n"));
        published_while_held = !first_returned.load();
        published.Open();
    });
    EXPECT_TRUE(entered.WaitFor(kAwait));
    EXPECT_TRUE(published.WaitFor(kAwait));

    std::thread helper([&] {
        std::this_thread::sleep_for(kLetItWait);
        release.Open();
    });
    std::thread::id racer_id;
    size_t at_racer_return = 0;
    size_t at_return = 0;
    if (closing == OutsideClosing::kUnsubscribe) {
        sub->Unsubscribe(id);
        at_return = log.Count();
    } else if (closing == OutsideClosing::kDestructor) {
        sub.reset();
        at_return = log.Count();
    } else {
        // A second cancel of the same id while the first one waits. It waits for the same drain
        // (owner ruling 2026-09-04, a racing cancel), so it too returns only after kClosing.
        std::thread racer([&] {
            racer_id = std::this_thread::get_id();
            std::this_thread::sleep_for(kLetItWait / 3);
            sub->Unsubscribe(id);
            at_racer_return = log.Count();
        });
        sub->Unsubscribe(id);
        at_return = log.Count();
        racer.join();
    }
    helper.join();
    publisher.join();
    EXPECT_TRUE(log.WaitAllReturned(kAwait));
    sub.reset();

    EXPECT_TRUE(published_while_held.load())
        << "rows 2-6 could not be published while the first callback was held (D10)";
    EXPECT_TRUE(released_by_helper.load())
        << "the first callback was not still held when the subscription was closed";
    EXPECT_EQ(at_return, 4u) << "the closing call returned before every delivery was made (L9)";
    if (closing == OutsideClosing::kRacingUnsubscribe) {
        EXPECT_EQ(at_racer_return, 4u) << "the racing Unsubscribe returned before kClosing";
    }
    std::vector<std::thread::id> foreign = {publisher_id, std::this_thread::get_id()};
    if (closing == OutsideClosing::kRacingUnsubscribe) foreign.push_back(racer_id);
    ExpectDeliveredInOrderOnOneBatchThread(log,
                                           {{BatchStatus::Reason::kRowLimit, 0, 2},
                                            {BatchStatus::Reason::kRowLimit, 2, 2},
                                            {BatchStatus::Reason::kRowLimit, 4, 2},
                                            {BatchStatus::Reason::kClosing, 6, 1}},
                                           foreign);
}

// Rung 4 (c). As rung 6 (c): the held kRowLimit callback unsubscribes from inside while a full
// window (x = 2, 3) is queued and x = 4 is pending, and stays held. The test thread then destroys
// the SubscriberArrow: the destructor returns only after the queued window and then kClosing.
void RunDestructorAfterInCallbackUnsubscribe() {
    DeliveryLog log;
    Latch entered, published, unsubscribed, release;
    std::atomic<uint64_t> id{0};
    std::unique_ptr<SubscriberArrow> sub;
    std::atomic<size_t> at_unsubscribe_return{0};
    std::atomic<bool> released_by_helper{false};
    std::atomic<bool> first_returned{false};
    log.hook = [&](size_t index, const DeliveryLog::Rec&) {
        if (index != 0) return;
        entered.Open();
        static_cast<void>(published.WaitFor(kHold));
        sub->Unsubscribe(id.load());
        at_unsubscribe_return = log.Count();
        unsubscribed.Open();
        released_by_helper = release.WaitFor(kHold);
        first_returned = true;
    };

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, TestSchema());
    sub = std::make_unique<SubscriberArrow>(provider);
    id = sub->Subscribe(kTopic, log.callback(), RowLimitOptions(2)).subscription_id;

    std::thread::id publisher_id;
    std::atomic<bool> published_while_held{false};
    std::thread publisher([&] {
        publisher_id = std::this_thread::get_id();
        pub.Publish(kTopic, MakeRow(0, "n"));
        pub.Publish(kTopic, MakeRow(1, "n"));
        static_cast<void>(entered.WaitFor(kAwait));
        for (int i = 2; i < 5; ++i) pub.Publish(kTopic, MakeRow(i, "n"));
        published_while_held = !first_returned.load();
        published.Open();
    });
    EXPECT_TRUE(unsubscribed.WaitFor(kAwait));

    std::thread helper([&] {
        std::this_thread::sleep_for(kLetItWait);
        release.Open();
    });
    sub.reset();
    const size_t at_return = log.Count();
    helper.join();
    publisher.join();
    EXPECT_TRUE(log.WaitAllReturned(kAwait));

    EXPECT_TRUE(published_while_held.load())
        << "rows 2-4 could not be published while the first callback was held (D10)";
    EXPECT_EQ(at_unsubscribe_return.load(), 1u)
        << "the in-callback Unsubscribe delivered a batch before it returned";
    EXPECT_TRUE(released_by_helper.load())
        << "the first callback was not still held when the SubscriberArrow was destroyed";
    EXPECT_EQ(at_return, 3u) << "~SubscriberArrow returned before every delivery was made (L9)";
    ExpectDeliveredInOrderOnOneBatchThread(log,
                                           {{BatchStatus::Reason::kRowLimit, 0, 2},
                                            {BatchStatus::Reason::kRowLimit, 2, 2},
                                            {BatchStatus::Reason::kClosing, 4, 1}},
                                           {publisher_id, std::this_thread::get_id()});
}

}  // namespace

// Rung 4 (L9, L12, L1): closing from outside goes through the same thread and the same order.
TEST(SubscriberArrowBatchTest, ClosingFromAnotherThreadDeliversQueuedWindowsThenClosing) {
    {
        SCOPED_TRACE("(a) Unsubscribe from another thread");
        RunClosingFromAnotherThread(OutsideClosing::kUnsubscribe);
    }
    {
        SCOPED_TRACE("(b) ~SubscriberArrow from another thread");
        RunClosingFromAnotherThread(OutsideClosing::kDestructor);
    }
    {
        SCOPED_TRACE("(c) ~SubscriberArrow after an in-callback Unsubscribe, a window queued");
        RunDestructorAfterInCallbackUnsubscribe();
    }
    {
        SCOPED_TRACE("(d) a racing second Unsubscribe of the same id");
        RunClosingFromAnotherThread(OutsideClosing::kRacingUnsubscribe);
    }
}

namespace {

enum class InsideClosing {
    kFromTimeout,
    kFromRowLimit,
    kWithQueuedWindow,
    kFromRowDelivery,
    kDestroyFromCallback,
};

// Rung 6. The first batch callback is held while a publisher thread sends rows after it; then the
// subscription is closed from inside that callback (a-c), from a thread delivering rows on the
// subscription's own transport while that callback is held (d; owner ruling 2026-11-11 (2)), or
// by destroying the SubscriberArrow inside that callback (e; owner ruling 2026-11-11 (1)). The
// closing call returns at once with nothing delivered inside it; after the held callback returns,
// the batch thread delivers any queued window, then kClosing, then nothing.
void RunClosingFromInside(InsideClosing closing) {
    DeliveryLog log;
    Latch entered, published, release;
    std::atomic<uint64_t> id{0};
    std::unique_ptr<SubscriberArrow> sub;
    std::atomic<bool> unsubscribe_returned_at_once{false};
    std::atomic<size_t> at_unsubscribe_return{0};
    std::atomic<bool> released_by_unsubscribe{false};
    std::atomic<bool> first_returned{false};
    const bool from_row_delivery = closing == InsideClosing::kFromRowDelivery;
    const bool destroy = closing == InsideClosing::kDestroyFromCallback;
    log.hook = [&](size_t index, const DeliveryLog::Rec&) {
        if (index != 0) return;
        entered.Open();
        if (from_row_delivery) {
            // Held until the row-delivery thread's Unsubscribe has returned.
            released_by_unsubscribe = release.WaitFor(kHold);
        } else {
            static_cast<void>(published.WaitFor(kHold));
            if (destroy) {
                sub.reset();  // the callback (this log) outlives what follows, as ruled
            } else {
                sub->Unsubscribe(id.load());
            }
            at_unsubscribe_return = log.Count();
            unsubscribe_returned_at_once = true;
        }
        first_returned = true;
    };

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, TestSchema());
    pub.CreateTopic(kOtherTopic, TestSchema());
    sub = std::make_unique<SubscriberArrow>(provider);
    // (e) starts from a kTimeout window too: destroying a SubscriberArrow inside a delivery on its
    // own transport is forbidden (2026-09-05), and a pre-D10 kRowLimit callback runs there.
    const bool timeout_first =
        closing == InsideClosing::kFromTimeout || from_row_delivery || destroy;
    SubscriberArrow::BatchOptions opt = RowLimitOptions(2);
    if (timeout_first) {
        // The first row alone closes at its deadline; (e) keeps max_rows = 2 so that the rows
        // published while the callback is held queue a full window.
        if (!destroy) opt.max_rows = 100;
        opt.timeout = std::chrono::milliseconds(200);
    }
    id = sub->Subscribe(kTopic, log.callback(), opt).subscription_id;

    // (d): a per-row subscription on another topic of the same transport. Its callback runs on the
    // publishing thread, inside the in-process delivery, and unsubscribes the batched one there.
    if (from_row_delivery) {
        static_cast<void>(sub->Subscribe(kOtherTopic, [&](ArrowRow, const Attachments&) {
            sub->Unsubscribe(id.load());
            at_unsubscribe_return = log.Count();
            unsubscribe_returned_at_once = !first_returned.load();
            release.Open();
        }));
    }

    // Rows left pending (k < max_rows) after the held window; (c) and (e) first queue a full one.
    const int more = closing == InsideClosing::kFromRowLimit ? 1 : 3;
    std::thread::id publisher_id;
    std::atomic<bool> published_while_held{false};
    std::thread publisher([&] {
        publisher_id = std::this_thread::get_id();
        int32_t next = 0;
        pub.Publish(kTopic, MakeRow(next++, "n"));
        if (!timeout_first) pub.Publish(kTopic, MakeRow(next++, "n"));  // the first full window
        static_cast<void>(entered.WaitFor(kAwait));
        for (int i = 0; i < more; ++i) pub.Publish(kTopic, MakeRow(next++, "n"));
        published_while_held = !first_returned.load();
        published.Open();
        if (from_row_delivery) pub.Publish(kOtherTopic, MakeRow(-1, "trigger"));
    });
    publisher.join();
    const bool queued = closing == InsideClosing::kWithQueuedWindow || destroy;
    EXPECT_TRUE(log.WaitFor(queued ? 3 : 2, kAwait));
    EXPECT_TRUE(log.WaitAllReturned(kAwait));
    if (!destroy) sub.reset();

    EXPECT_TRUE(published_while_held.load())
        << "rows could not be published while the first callback was held (D10)";
    EXPECT_TRUE(unsubscribe_returned_at_once.load())
        << (from_row_delivery ? "the row-delivery thread's Unsubscribe waited for the held batch "
                                "callback instead of returning at once"
                              : "the in-callback closing call did not return");
    EXPECT_EQ(at_unsubscribe_return.load(), 1u)
        << "a batch was delivered before the closing call returned";
    if (from_row_delivery) {
        EXPECT_TRUE(released_by_unsubscribe.load())
            << "the batch callback was released by its own timeout, not by the returned Unsubscribe";
    }
    std::vector<Expected> expected;
    switch (closing) {
        case InsideClosing::kFromTimeout:
        case InsideClosing::kFromRowDelivery:
            expected = {{BatchStatus::Reason::kTimeout, 0, 1},
                        {BatchStatus::Reason::kClosing, 1, 3}};
            break;
        case InsideClosing::kFromRowLimit:
            expected = {{BatchStatus::Reason::kRowLimit, 0, 2},
                        {BatchStatus::Reason::kClosing, 2, 1}};
            break;
        case InsideClosing::kWithQueuedWindow:
            expected = {{BatchStatus::Reason::kRowLimit, 0, 2},
                        {BatchStatus::Reason::kRowLimit, 2, 2},
                        {BatchStatus::Reason::kClosing, 4, 1}};
            break;
        case InsideClosing::kDestroyFromCallback:
            expected = {{BatchStatus::Reason::kTimeout, 0, 1},
                        {BatchStatus::Reason::kRowLimit, 1, 2},
                        {BatchStatus::Reason::kClosing, 3, 1}};
            break;
    }
    std::vector<std::thread::id> foreign = {publisher_id, std::this_thread::get_id()};
    ExpectDeliveredInOrderOnOneBatchThread(log, expected, foreign);
}

}  // namespace

// Rung 6 (L9 as revised, L11, L12; owner rulings "Deliver after it returns", "Queued batch, then
// closing", and 2026-11-11 (2) and (1) for cases (d) and (e)).
TEST(SubscriberArrowBatchTest, UnsubscribeFromInsideACallbackNeverDeliversNested) {
    {
        SCOPED_TRACE("(a) from a kTimeout callback, 3 rows pending");
        RunClosingFromInside(InsideClosing::kFromTimeout);
    }
    {
        SCOPED_TRACE("(b) from a kRowLimit callback, 1 row pending");
        RunClosingFromInside(InsideClosing::kFromRowLimit);
    }
    {
        SCOPED_TRACE("(c) from a kRowLimit callback, a full window queued and 1 row pending");
        RunClosingFromInside(InsideClosing::kWithQueuedWindow);
    }
    {
        SCOPED_TRACE("(d) from a thread delivering rows on the same transport, a callback held");
        RunClosingFromInside(InsideClosing::kFromRowDelivery);
    }
    {
        SCOPED_TRACE("(e) ~SubscriberArrow inside a kTimeout callback, a full window queued");
        RunClosingFromInside(InsideClosing::kDestroyFromCallback);
    }
}

// Guard (green before and after; BC-5). A batch callback unsubscribes, which reaches the in-process
// provider's Unsubscribe, while another thread is inside Publish holding that provider's mutex (a
// per-row delivery on another topic that dwells). Both calls return; the batch callback ran for the
// one window, then for exactly one empty closing delivery, last (ruling 2026-11-14).
TEST(SubscriberArrowBatchTest, InCallbackUnsubscribeWhilePublishingDoesNotDeadlock) {
    DeliveryLog log;
    Latch entered, row_delivery_inside, unsubscribed, published;
    std::atomic<uint64_t> id{0};
    std::unique_ptr<SubscriberArrow> sub;
    std::atomic<bool> overlapped{false};
    log.hook = [&](size_t index, const DeliveryLog::Rec&) {
        if (index != 0) return;
        entered.Open();
        overlapped = row_delivery_inside.WaitFor(kHold);
        sub->Unsubscribe(id.load());
        unsubscribed.Open();
    };

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, TestSchema());
    pub.CreateTopic(kOtherTopic, TestSchema());
    sub = std::make_unique<SubscriberArrow>(provider);
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100;
    opt.timeout = std::chrono::milliseconds(50);
    id = sub->Subscribe(kTopic, log.callback(), opt).subscription_id;
    static_cast<void>(sub->Subscribe(kOtherTopic, [&](ArrowRow, const Attachments&) {
        row_delivery_inside.Open();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // holds the provider's mutex
    }));

    pub.Publish(kTopic, MakeRow(0, "n"));  // one row; its window closes at the deadline
    EXPECT_TRUE(entered.WaitFor(kAwait));
    std::thread publisher([&] {
        pub.Publish(kOtherTopic, MakeRow(1, "dwell"));
        published.Open();
    });
    EXPECT_TRUE(unsubscribed.WaitFor(kAwait)) << "the in-callback Unsubscribe did not return";
    EXPECT_TRUE(published.WaitFor(kAwait)) << "the Publish did not return";
    publisher.join();
    EXPECT_TRUE(log.WaitAllReturned(kAwait));
    sub.reset();

    EXPECT_TRUE(overlapped.load());
    const auto recs = log.Snapshot();
    ASSERT_EQ(recs.size(), 2u) << "the window, then exactly one closing delivery";
    EXPECT_EQ(recs[0].reason, BatchStatus::Reason::kTimeout);
    EXPECT_EQ(recs[0].num_rows, 1);
    EXPECT_EQ(recs[1].reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(recs[1].num_rows, 0) << "an empty closing is a zero-row batch here (L7)";
    EXPECT_EQ(recs[1].dropped, 0);
}

// Guard (BC-1): fixed seeds; max_rows 1-3, timeout 1 ms; a publisher thread sends rows in bursts;
// the subscription is closed from inside a callback or from outside at a seeded point. Every
// callback runs on one thread that is not a publishing or closing thread, never two at once,
// windows arrive in order (first x strictly increasing), and exactly one kClosing comes, last
// (ruling 2026-11-14: even when the close finds the window idle).
TEST(SubscriberArrowBatchTest, DeliveryOrderStress) {
    for (uint32_t seed = 1; seed <= 16; ++seed) {
        std::mt19937 rng(seed);
        const int64_t max_rows = 1 + static_cast<int64_t>(rng() % 3);
        const int rows = 20 + static_cast<int>(rng() % 21);
        const int close_at = static_cast<int>(rng() % static_cast<uint32_t>(rows));
        const bool inside = rng() % 2 == 0;
        std::vector<char> pause_after(static_cast<size_t>(rows));
        for (auto& p : pause_after) p = rng() % 3 == 0 ? 1 : 0;
        SCOPED_TRACE("seed " + std::to_string(seed) + ": max_rows " + std::to_string(max_rows) +
                     ", " + std::to_string(rows) + " rows, closed " +
                     (inside ? "inside" : "outside") + " at x " + std::to_string(close_at));

        DeliveryLog log;
        Latch reached, closed;
        std::atomic<uint64_t> id{0};
        std::unique_ptr<SubscriberArrow> sub;
        std::atomic<bool> fired{false};
        if (inside) {
            log.hook = [&](size_t, const DeliveryLog::Rec& rec) {
                if (rec.last_x < close_at || fired.exchange(true)) return;
                sub->Unsubscribe(id.load());
                closed.Open();
            };
        }

        auto provider = std::make_shared<InProcessPubSubProvider>();
        PublisherArrow pub(provider);
        pub.CreateTopic(kTopic, TestSchema());
        sub = std::make_unique<SubscriberArrow>(provider);
        SubscriberArrow::BatchOptions opt;
        opt.max_rows = max_rows;
        opt.timeout = std::chrono::milliseconds(1);
        id = sub->Subscribe(kTopic, log.callback(), opt).subscription_id;

        std::thread::id publisher_id;
        std::thread publisher([&] {
            publisher_id = std::this_thread::get_id();
            for (int x = 0; x < rows; ++x) {
                pub.Publish(kTopic, MakeRow(x, "n"));
                if (x == close_at) reached.Open();
                if (pause_after[static_cast<size_t>(x)] != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
        });
        if (inside) {
            EXPECT_TRUE(closed.WaitFor(kSeedAwait)) << "no delivery reached x " << close_at;
        } else {
            EXPECT_TRUE(reached.WaitFor(kSeedAwait));
            sub->Unsubscribe(id.load());  // while the publisher keeps publishing
        }
        publisher.join();
        EXPECT_TRUE(log.WaitAllReturned(kSeedAwait));
        sub.reset();

        const auto recs = log.Snapshot();
        EXPECT_LE(log.max_in_flight.load(), 1) << "two batch callbacks ran at once (L12)";
        EXPECT_LE(log.max_depth.load(), 1) << "a batch was delivered inside a batch callback";
        ASSERT_FALSE(recs.empty());
        EXPECT_EQ(
            std::count_if(recs.begin(), recs.end(),
                          [](const auto& r) { return r.reason == BatchStatus::Reason::kClosing; }),
            1)
            << "not exactly one kClosing (L9)";
        int32_t previous_first_x = -1;
        for (size_t i = 0; i < recs.size(); ++i) {
            SCOPED_TRACE("delivery " + std::to_string(i) + " (" + ReasonName(recs[i].reason) + ")");
            EXPECT_EQ(recs[i].thread, recs[0].thread) << "a second callback thread (L12)";
            EXPECT_NE(recs[i].thread, publisher_id) << "a callback on the publishing thread (D10)";
            EXPECT_NE(recs[i].thread, std::this_thread::get_id())
                << "a callback on the closing thread (D10)";
            if (recs[i].first_x >= 0) {
                EXPECT_GT(recs[i].first_x, previous_first_x) << "windows out of order (L12)";
                previous_first_x = recs[i].first_x;
            }
            if (recs[i].reason == BatchStatus::Reason::kClosing) {
                EXPECT_EQ(i, recs.size() - 1) << "a delivery after kClosing (L9)";
            }
        }
        if (HasFailure()) return;  // one failed seed is enough, and keeps a red row short
    }
}

// Guard (BC-2; green before and after): fixed seeds; rows with no usable schema yet, then
// well-formed and malformed rows; ended from outside, or from inside the first delivery after the
// publisher returned from its last Publish. Rows received plus rows reported lost equal rows sent
// (L1, L2). The over-budget row (L1 violation 2) is the heavy test's.
TEST(SubscriberArrowBatchTest, RowsConservedAtEveryEnding) {
    for (uint32_t seed = 1; seed <= 12; ++seed) {
        std::mt19937 rng(seed);
        const int64_t max_rows = 1 + static_cast<int64_t>(rng() % 4);
        const int schema_less = static_cast<int>(rng() % 4);
        const int rows = 10 + static_cast<int>(rng() % 21);
        const bool inside = rng() % 2 == 0;
        SCOPED_TRACE("seed " + std::to_string(seed) + ": max_rows " + std::to_string(max_rows) +
                     ", " + std::to_string(schema_less) + " schema-less + " +
                     std::to_string(rows) + " rows, ended " + (inside ? "inside" : "outside"));

        DeliveryLog log;
        Latch all_published, closed;
        std::atomic<uint64_t> id{0};
        std::unique_ptr<SubscriberArrow> sub;
        std::atomic<bool> fired{false};
        if (inside) {
            log.hook = [&](size_t, const DeliveryLog::Rec&) {
                if (!all_published.IsOpen() || fired.exchange(true)) return;
                sub->Unsubscribe(id.load());
                closed.Open();
            };
        }

        auto mock = std::make_shared<MockProvider>();  // published from this thread only
        PublisherArrow pub(mock);
        sub = std::make_unique<SubscriberArrow>(mock);
        SubscriberArrow::BatchOptions opt;
        opt.max_rows = max_rows;
        opt.timeout = std::chrono::milliseconds(5);
        id = sub->Subscribe(kTopic, log.callback(), opt).subscription_id;

        const auto malformed = [](WriteBuffer& buf) { buf.AppendByte(0x00); };
        int64_t sent = 0;
        for (int i = 0; i < schema_less; ++i, ++sent) pub.PublishDirect(kTopic, malformed);
        pub.CreateTopic(kTopic, TestSchema());
        for (int i = 0; i < rows; ++i, ++sent) {
            if (rng() % 10 < 3) {
                pub.PublishDirect(kTopic, malformed);
            } else {
                pub.Publish(kTopic, MakeRow(i, "n"));
            }
            if (rng() % 4 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (inside) {
            // A last lost row leaves a window pending (a drop never fills one), so a delivery is
            // owed after the last Publish has returned.
            pub.PublishDirect(kTopic, malformed);
            ++sent;
            all_published.Open();
            EXPECT_TRUE(closed.WaitFor(kSeedAwait)) << "no delivery after the last Publish";
        } else {
            sub->Unsubscribe(id.load());
        }
        EXPECT_TRUE(log.WaitAllReturned(kSeedAwait));
        sub.reset();

        int64_t accounted = 0;
        for (const auto& r : log.Snapshot()) {
            accounted += std::max<int64_t>(r.num_rows, 0) + r.dropped;
        }
        EXPECT_EQ(accounted, sent) << "rows received + rows reported lost != rows sent";
        if (HasFailure()) return;  // one failed seed is enough, and keeps a red row short
    }
}

// Guard (code review 4b S1): a batched Subscribe that throws leaves no batch thread behind. Here
// it is refused with kReentrantCall, made from inside a per-row delivery for a topic not yet
// subscribed. The callback it was handed is released, so nothing it captured outlives the throw,
// and it is never called (ruling 2026-11-14 "a refused batched subscribe never delivers"; the
// counter is shared by value, so a late call cannot touch this frame).
TEST(SubscriberArrowBatchTest, FailedBatchedSubscribeReleasesItsCallback) {
    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, TestSchema());
    pub.CreateTopic(kOtherTopic, TestSchema());
    SubscriberArrow sub(provider);

    std::weak_ptr<int> captured;
    std::atomic<bool> refused{false};
    const auto calls = std::make_shared<std::atomic<int>>(0);
    static_cast<void>(sub.Subscribe(kTopic, [&](ArrowRow, const Attachments&) {
        auto token = std::make_shared<int>(0);
        captured = token;
        try {
            static_cast<void>(sub.Subscribe(
                kOtherTopic,
                [token, calls](std::shared_ptr<arrow::RecordBatch>, std::vector<Attachments>,
                               BatchStatus) { ++*calls; },
                RowLimitOptions(2)));
        } catch (const PubSubError& e) {
            refused = e.status() == PubSubStatus::kReentrantCall;
        }
    }));
    pub.Publish(kTopic, MakeRow(0, "n"));
    ASSERT_TRUE(refused.load()) << "the re-entrant batched Subscribe was not refused";

    const auto until = std::chrono::steady_clock::now() + kAwait;
    while (!captured.expired() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(captured.expired())
        << "the failed Subscribe's batch thread still holds the callback it was given";
    EXPECT_EQ(calls->load(), 0) << "the refused Subscribe's callback was called";
}

// ---------------------------------------------------------------------------
// D7O-1 (L15, BC-8): batch options out of range are refused, never adjusted
// ---------------------------------------------------------------------------

// Rung 1 + 2. Every out-of-range arm is refused with kInvalidArgument, and what it was handed is
// released before the exception reaches the caller (no polling: the weak_ptr is read straight after
// the catch). Nothing reaches the provider and nothing is ever delivered; a valid Subscribe on the
// same topic afterwards still works, which is what makes "nothing delivered" mean something.
TEST(SubscriberArrowBatchTest, OutOfRangeBatchOptionsAreRefusedAndLeaveNothingBehind) {
    struct Arm {
        const char* name;
        int64_t max_rows;
        std::chrono::milliseconds timeout;
    };
    const std::chrono::milliseconds kDefaultTimeout = SubscriberArrow::BatchOptions{}.timeout;
    const std::vector<Arm> arms = {
        {"max_rows 0", 0, kDefaultTimeout},
        {"max_rows -1", -1, kDefaultTimeout},
        {"max_rows INT64_MIN", std::numeric_limits<int64_t>::min(), kDefaultTimeout},
        {"timeout -1 ms", 8000, std::chrono::milliseconds(-1)},
        {"timeout milliseconds::min()", 8000, std::chrono::milliseconds::min()},
        {"both out of range", 0, std::chrono::milliseconds(-1)},
    };

    std::deque<BatchSink> sinks;  // declared first: outlives the subscriber on every path
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());
    ASSERT_EQ(mock->subscribe_with_options_count, 0);

    for (const Arm& arm : arms) {
        SCOPED_TRACE(arm.name);
        sinks.emplace_back();
        BatchSink& sink = sinks.back();

        // The token lives only inside the callback: nothing here keeps it alive past the call.
        std::weak_ptr<int> token;
        SubscriberArrow::RecordBatchCallback cb;
        {
            auto strong = std::make_shared<int>(0);
            token = strong;
            auto inner = sink.callback();
            cb = [strong, inner](std::shared_ptr<arrow::RecordBatch> batch,
                                 std::vector<Attachments> att, BatchStatus status) {
                inner(std::move(batch), std::move(att), status);
            };
        }

        SubscriberArrow::BatchOptions opt;
        opt.max_rows = arm.max_rows;
        opt.timeout = arm.timeout;
        bool refused = false;
        bool accepted = false;
        uint64_t accepted_id = 0;
        try {
            const auto result = sub.Subscribe(kTopic, std::move(cb), opt);
            ADD_FAILURE() << "the out-of-range options were accepted";
            accepted = true;
            accepted_id = result.subscription_id;
        } catch (const PubSubError& e) {
            refused = true;
            EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
        }
        EXPECT_TRUE(refused);
        // Straight after the call, no waiting: a refused Subscribe has already let the callback go.
        // (cb itself was moved from, so only the batcher could still hold it.)
        EXPECT_TRUE(token.expired()) << "a callback is still held after the call returned";
        // Only the milliseconds::min() arm reaches signed overflow on the old tree (the deadline
        // sum at the first row), so only it is closed before the Publish. The other arms stay live
        // through the observation window below.
        if (accepted && arm.timeout == std::chrono::milliseconds::min()) {
            sub.Unsubscribe(accepted_id);
        }
    }

    // Nothing was subscribed at the provider.
    std::cerr << "[ D7O-1 ] observed subscribe_with_options_count after the refusals = "
              << mock->subscribe_with_options_count << "\n";
    EXPECT_EQ(mock->subscribe_with_options_count, 0);

    pub.Publish(kTopic, MakeRow(1, "a"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // one-sided observation window
    for (size_t i = 0; i < sinks.size(); ++i) {
        std::lock_guard<std::mutex> lk(sinks[i].mu);
        EXPECT_TRUE(sinks[i].deliveries.empty())
            << "arm " << arms[i].name << " received " << sinks[i].deliveries.size()
            << " deliveries";
    }

    // Positive control: the topic is alive, and a valid Subscribe reaches the provider.
    BatchSink good;
    const auto good_result = sub.Subscribe(kTopic, good.callback(), RowLimitOptions(1));
    EXPECT_EQ(mock->subscribe_with_options_count, 1);
    pub.Publish(kTopic, MakeRow(2, "b"));
    ASSERT_TRUE(good.WaitFor(1, std::chrono::seconds(2)));
    {
        std::lock_guard<std::mutex> lk(good.mu);
        ASSERT_GE(good.deliveries.size(), 1u);
        EXPECT_EQ(good.deliveries[0].num_rows, 1);
        EXPECT_EQ(good.deliveries[0].status.reason, BatchStatus::Reason::kRowLimit);
    }
    sub.Unsubscribe(good_result.subscription_id);
}

// Guard (green both ways): the new check accepts both lower bounds and the batcher still gets them
// unchanged. A check written `<= 0`, or `max_rows < 2`, turns this red.
TEST(SubscriberArrowBatchTest, InRangeBatchOptionsAreUsedAsGiven) {
    // Row-count arm: max_rows 1 closes a window on every row.
    {
        BatchSink sink;  // declared first: it outlives the subscriber (d7 L9 closing)
        auto mock = std::make_shared<MockProvider>();
        PublisherArrow pub(mock);
        SubscriberArrow sub(mock);
        pub.CreateTopic(kTopic, TestSchema());
        static_cast<void>(sub.Subscribe(kTopic, sink.callback(), RowLimitOptions(1)));
        for (int i = 0; i < 3; ++i) pub.Publish(kTopic, MakeRow(i, "n"));
        ASSERT_TRUE(sink.WaitFor(3, std::chrono::seconds(2)));
        std::lock_guard<std::mutex> lk(sink.mu);
        ASSERT_GE(sink.deliveries.size(), 3u);
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(sink.deliveries[i].num_rows, 1) << "batch " << i;
            EXPECT_EQ(sink.deliveries[i].status.reason, BatchStatus::Reason::kRowLimit);
            EXPECT_EQ(sink.deliveries[i].status.rows_dropped, 0);
        }
    }
    // Zero-timeout arm: accepted, and the window still waits at least the 1 ms floor.
    {
        // Declared first: what the callback captures outlives the subscriber (d7 L9 closing).
        std::mutex mu;
        std::condition_variable cv;
        std::vector<std::chrono::steady_clock::time_point> arrived;
        std::vector<BatchStatus> statuses;
        std::vector<int64_t> rows;
        auto mock = std::make_shared<MockProvider>();
        PublisherArrow pub(mock);
        SubscriberArrow sub(mock);
        pub.CreateTopic(kTopic, TestSchema());
        SubscriberArrow::BatchOptions opt;
        opt.max_rows = 8000;
        opt.timeout = std::chrono::milliseconds(0);
        static_cast<void>(sub.Subscribe(
            kTopic,
            [&](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments>,
                BatchStatus status) {
                std::lock_guard<std::mutex> lk(mu);
                arrived.push_back(std::chrono::steady_clock::now());
                statuses.push_back(status);
                rows.push_back(batch ? batch->num_rows() : -1);
                cv.notify_all();
            },
            opt));
        const auto t0 = std::chrono::steady_clock::now();
        pub.Publish(kTopic, MakeRow(1, "a"));
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(2), [&] { return !arrived.empty(); }));
        EXPECT_EQ(rows[0], 1);
        EXPECT_EQ(statuses[0].reason, BatchStatus::Reason::kTimeout);
        EXPECT_GE(arrived[0] - t0, std::chrono::milliseconds(1));
    }
}

// ---------------------------------------------------------------------------
// D7O-2 (L4, L5, L1, L6, L9): no timeout, and a timeout too long for the clock
// ---------------------------------------------------------------------------

namespace {

// The observation window is 200 ms and one-sided: a test that sees nothing in it proves nothing
// by itself, which is why ShortTimeoutControlSeesTheDeadlineInEveryBody runs the same bodies on a
// 10 ms timeout and must see a delivery.
constexpr std::chrono::milliseconds kObservationWindow{200};

enum class Body {
    kSevenRowsMaxThree,  // (a) declared schema, max_rows 3, 7 Publish (rows 0-6)
    kNeverDeclaredFive,  // (b) topic never declared, max_rows 2, 5 PublishDirect rows
    kMalformedFive,      // (c) declared schema, max_rows 2, 5 malformed rows
};

enum class NoTimeoutEnding { kUnsubscribe, kDestroy };

// One run of one body: subscribes with `timeout`, publishes the body's rows, and holds the
// subscription until End(). The caller declares its BatchSink first, so the sink outlives the
// subscriber on every path (4b nit). The Mock delivers synchronously, so every row has reached
// the batcher's AddRow when the constructor returns.
class BodyRun {
   public:
    BodyRun(Body body, std::chrono::milliseconds timeout, BatchSink& sink)
        : mock_(std::make_shared<MockProvider>()), pub_(mock_) {
        sub_.emplace(mock_);
        if (body != Body::kNeverDeclaredFive) pub_.CreateTopic(kTopic, TestSchema());
        SubscriberArrow::BatchOptions opt;
        opt.max_rows = body == Body::kSevenRowsMaxThree ? 3 : 2;
        opt.timeout = timeout;
        id_ = sub_->Subscribe(kTopic, sink.callback(), opt).subscription_id;
        const auto malformed = [](WriteBuffer& buf) { buf.AppendByte(0x00); };
        switch (body) {
            case Body::kSevenRowsMaxThree:
                for (int i = 0; i < 7; ++i) pub_.Publish(kTopic, MakeRow(i, "n"));
                break;
            case Body::kNeverDeclaredFive:
            case Body::kMalformedFive:
                for (int i = 0; i < 5; ++i) pub_.PublishDirect(kTopic, malformed);
                break;
        }
    }

    // Ends the subscription from outside any callback: everything owed is delivered before this
    // returns (L9).
    void End(NoTimeoutEnding ending) {
        if (ending == NoTimeoutEnding::kUnsubscribe) {
            sub_->Unsubscribe(id_);
        } else {
            sub_.reset();
        }
    }

   private:
    std::shared_ptr<MockProvider> mock_;
    PublisherArrow pub_;
    std::optional<SubscriberArrow> sub_;
    uint64_t id_ = 0;
};

void Observe() { std::this_thread::sleep_for(kObservationWindow); }

size_t Count(BatchSink& sink) {
    std::lock_guard<std::mutex> lk(sink.mu);
    return sink.deliveries.size();
}

// "kRowLimit:3 rows, kTimeout:1 rows (dropped 0)": what was delivered, for a failure message.
std::string Describe(BatchSink& sink) {
    std::lock_guard<std::mutex> lk(sink.mu);
    std::string out;
    for (const auto& d : sink.deliveries) {
        const char* reason = d.status.reason == BatchStatus::Reason::kRowLimit  ? "kRowLimit"
                             : d.status.reason == BatchStatus::Reason::kTimeout ? "kTimeout"
                                                                                : "kClosing";
        out += std::string(out.empty() ? "" : ", ") + reason + ":" + std::to_string(d.num_rows) +
               " rows (dropped " + std::to_string(d.status.rows_dropped) + ")";
    }
    return out;
}

int32_t XAt(const BatchSink::Delivery& d, int64_t row) {
    return std::static_pointer_cast<arrow::Int32Array>(d.batch->column(0))->Value(row);
}

// Rung 2 / rung 3 assertions for body (a): two kRowLimit batches (rows 0-2, 3-5) and then
// nothing for a whole observation window; the ending delivers exactly one more, kClosing, row 6;
// and nothing follows it (L9).
void ExpectBodyAClosesOnlyWhenFullOrAtClosing(std::chrono::milliseconds timeout,
                                               NoTimeoutEnding ending) {
    BatchSink sink;
    BodyRun run(Body::kSevenRowsMaxThree, timeout, sink);

    ASSERT_TRUE(sink.WaitFor(2, std::chrono::seconds(2)));
    Observe();
    ASSERT_EQ(Count(sink), 2u) << "a batch arrived that was neither full nor the closing one: "
                               << Describe(sink);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        for (size_t i = 0; i < 2; ++i) {
            SCOPED_TRACE(i);
            const auto& d = sink.deliveries[i];
            EXPECT_EQ(d.status.reason, BatchStatus::Reason::kRowLimit);
            EXPECT_EQ(d.status.rows_dropped, 0);
            ASSERT_EQ(d.num_rows, 3);
            for (int64_t r = 0; r < 3; ++r) {
                EXPECT_EQ(XAt(d, r), static_cast<int32_t>(3 * i + r));
            }
        }
    }

    run.End(ending);
    ASSERT_EQ(Count(sink), 3u);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        const auto& d = sink.deliveries[2];
        EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
        EXPECT_EQ(d.status.rows_dropped, 0);
        ASSERT_EQ(d.num_rows, 1);
        EXPECT_EQ(XAt(d, 0), 6);
    }
    Observe();
    EXPECT_EQ(Count(sink), 3u) << "something was delivered after the closing batch (L9)";
}

// Rung 2 assertions for bodies (b) and (c), where every row is lost: nothing for a whole
// observation window (lost rows never count toward max_rows, so only the ending can close the
// window), then exactly one batch from the ending, reason kClosing, carrying all five losses (L1,
// L6, L7). (b) has no usable schema, so its batch is null with no attachments; (c) has one, so its
// batch has zero rows.
void ExpectOnlyTheEndingReportsTheLostRows(Body body, std::chrono::milliseconds timeout,
                                           NoTimeoutEnding ending) {
    BatchSink sink;
    BodyRun run(body, timeout, sink);

    Observe();
    ASSERT_EQ(Count(sink), 0u) << "a batch arrived before the ending: " << Describe(sink);

    run.End(ending);
    ASSERT_EQ(Count(sink), 1u) << Describe(sink);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        const auto& d = sink.deliveries[0];
        EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
        EXPECT_EQ(d.status.rows_dropped, 5);
        if (body == Body::kNeverDeclaredFive) {
            EXPECT_EQ(d.batch, nullptr);
            EXPECT_TRUE(d.attachments.empty());
        } else {
            ASSERT_NE(d.batch, nullptr);
            EXPECT_EQ(d.num_rows, 0);
        }
    }
    Observe();
    EXPECT_EQ(Count(sink), 1u) << "something was delivered after the closing batch (L9)";
}

// Waits until a kTimeout delivery has arrived. Returns false after 2 s.
bool WaitForTimeoutDelivery(BatchSink& sink) {
    std::unique_lock<std::mutex> lk(sink.mu);
    return sink.cv.wait_for(lk, std::chrono::seconds(2), [&] {
        for (const auto& d : sink.deliveries) {
            if (d.status.reason == BatchStatus::Reason::kTimeout) return true;
        }
        return false;
    });
}

}  // namespace

// Rung 2 (L4, L5, L1, L6, L7, L9). kNoTimeout: a window closes only when full, on the offset
// budget, or at the ending; never on a clock. Subscribe must also accept the value (BC-8).
//  (a) full windows arrive at once, nothing else within the observation window, and the ending
//      (Unsubscribe, or the destructor) delivers the one remaining row as kClosing;
//  (b) and (c): rows that were all lost wait for the ending and are reported there.
// Red on `9cd04de`: it does not compile, because kNoTimeout does not exist. That these assertions
// can fail behaviourally is shown by ShortTimeoutControlSeesTheDeadlineInEveryBody (same bodies,
// 10 ms) and by the first two arms of TimeoutTooLongForTheClockBehavesAsNoTimeout.
TEST(SubscriberArrowBatchTest, NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing) {
    constexpr auto kNever = SubscriberArrow::BatchOptions::kNoTimeout;
    {
        SCOPED_TRACE("(a) ended by Unsubscribe");
        ExpectBodyAClosesOnlyWhenFullOrAtClosing(kNever, NoTimeoutEnding::kUnsubscribe);
    }
    {
        SCOPED_TRACE("(a) ended by ~SubscriberArrow");
        ExpectBodyAClosesOnlyWhenFullOrAtClosing(kNever, NoTimeoutEnding::kDestroy);
    }
    {
        SCOPED_TRACE("(b) never declared, ended by Unsubscribe");
        ExpectOnlyTheEndingReportsTheLostRows(Body::kNeverDeclaredFive, kNever,
                                              NoTimeoutEnding::kUnsubscribe);
    }
    {
        SCOPED_TRACE("(c) malformed rows, ended by Unsubscribe");
        ExpectOnlyTheEndingReportsTheLostRows(Body::kMalformedFive, kNever,
                                              NoTimeoutEnding::kUnsubscribe);
    }
}

// Rung 3 (L5). A timeout too long for the clock means no deadline, never an early or a wrapped
// one. Body (a), ended by Unsubscribe, with the assertions of rung 2's (a) arm. Two arms are red
// on `9cd04de` (the deadline sum overflows); the 100-year arm is a declared GUARD, green both
// ways: its sum is representable, so its deadline is real and lies centuries away. It forbids a
// "fix" that refuses huge values, or one that misjudges where the clock's reach ends.
TEST(SubscriberArrowBatchTest, TimeoutTooLongForTheClockBehavesAsNoTimeout) {
    {
        // The literal, not kNoTimeout: converting INT64_MAX ms to nanoseconds wraps to -1 ms.
        SCOPED_TRACE("milliseconds::max()");
        ExpectBodyAClosesOnlyWhenFullOrAtClosing(std::chrono::milliseconds::max(),
                                                 NoTimeoutEnding::kUnsubscribe);
    }
    {
        // Converts exactly, but last_flush_ + timeout_ overflows to a deadline in the past.
        SCOPED_TRACE("the clock's own maximum, in milliseconds");
        ExpectBodyAClosesOnlyWhenFullOrAtClosing(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::duration::max()),
            NoTimeoutEnding::kUnsubscribe);
    }
    {
        // GUARD arm: 100 years (3 155 760 000 000 ms) is representable; green before and after.
        SCOPED_TRACE("100 years (guard)");
        ExpectBodyAClosesOnlyWhenFullOrAtClosing(std::chrono::milliseconds(3'155'760'000'000LL),
                                                 NoTimeoutEnding::kUnsubscribe);
    }
}

// Control (green both ways, declared). Bodies (a), (b) and (c) with a 10 ms timeout: each gets a
// kTimeout delivery within 2 s, so the observation window and the helper do see a deadline that
// exists, and rung 2's "nothing within the window" is not true by construction. After Unsubscribe
// every row is accounted for. (DEBT-1: which rows the kTimeout batch holds depends on timing, so
// only its existence and what it can be made of are asserted: a lost-rows batch for (b) and (c),
// a batch with at least one row for (a).)
TEST(SubscriberArrowBatchTest, ShortTimeoutControlSeesTheDeadlineInEveryBody) {
    struct Arm {
        const char* name;
        Body body;
    };
    for (const Arm& arm : {Arm{"(a) seven rows, max_rows 3", Body::kSevenRowsMaxThree},
                           Arm{"(b) never declared", Body::kNeverDeclaredFive},
                           Arm{"(c) malformed rows", Body::kMalformedFive}}) {
        SCOPED_TRACE(arm.name);
        BatchSink sink;
        BodyRun run(arm.body, std::chrono::milliseconds(10), sink);

        ASSERT_TRUE(WaitForTimeoutDelivery(sink)) << "no kTimeout delivery within 2 s";
        run.End(NoTimeoutEnding::kUnsubscribe);

        std::lock_guard<std::mutex> lk(sink.mu);
        int64_t rows = 0;
        int64_t dropped = 0;
        bool timeout_batch_has_content = false;
        for (const auto& d : sink.deliveries) {
            rows += d.num_rows > 0 ? d.num_rows : 0;
            dropped += d.status.rows_dropped;
            if (d.status.reason != BatchStatus::Reason::kTimeout) continue;
            if (arm.body == Body::kSevenRowsMaxThree) {
                timeout_batch_has_content |= d.num_rows >= 1;
            } else {
                timeout_batch_has_content |= d.status.rows_dropped >= 1;
            }
        }
        EXPECT_TRUE(timeout_batch_has_content);
        if (arm.body == Body::kSevenRowsMaxThree) {
            EXPECT_EQ(rows, 7);
            EXPECT_EQ(dropped, 0);
        } else {
            EXPECT_EQ(rows, 0);
            EXPECT_EQ(dropped, 5);
        }
    }
}

// ---------------------------------------------------------------------------
// D7O-3 (L15, L8, BC-8): any row count is used as given
// ---------------------------------------------------------------------------

namespace {

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr auto kNoTimeoutValue = SubscriberArrow::BatchOptions::kNoTimeout;

struct Totals {
    int64_t rows = 0;     // sum of num_rows over non-null batches
    int64_t dropped = 0;  // sum of rows_dropped
    size_t nulls = 0;     // null batches
};

Totals Sum(BatchSink& sink) {
    std::lock_guard<std::mutex> lk(sink.mu);
    Totals t;
    for (const auto& d : sink.deliveries) {
        if (d.batch) {
            t.rows += d.num_rows;
        } else {
            ++t.nulls;
        }
        t.dropped += d.status.rows_dropped;
    }
    return t;
}

// Waits (up to `timeout`) until `n` rows are accounted for, delivered or counted lost. A row that is
// reported lost counts, so a run that loses its rows fails on its assertions at once instead of
// waiting out the clock; a run that delivers them all leaves exactly as the plain wait would.
bool WaitForAccounted(BatchSink& sink, int64_t n, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(sink.mu);
    return sink.cv.wait_for(lk, timeout, [&] {
        int64_t accounted = 0;
        for (const auto& d : sink.deliveries) {
            accounted += (d.batch ? d.num_rows : 0) + d.status.rows_dropped;
        }
        return accounted >= n;
    });
}

// The x column, read across the deliveries in order, is first, first + 1, ...: no row lost, none
// repeated, none reordered. Null batches are skipped (the callers' own checks report them).
void ExpectXsRunFrom(BatchSink& sink, int32_t first) {
    std::lock_guard<std::mutex> lk(sink.mu);
    int32_t expected = first;
    for (const auto& d : sink.deliveries) {
        if (!d.batch) continue;
        for (int64_t r = 0; r < d.num_rows; ++r) {
            ASSERT_EQ(XAt(d, r), expected) << "row " << (expected - first) << " out of place";
            ++expected;
        }
    }
}

// (a) A huge max_rows with a finite timeout: the rows arrive on the deadline.
void ExpectHugeMaxRowsWithTimeoutDelivers() {
    BatchSink sink;  // declared first: outlives the subscriber on every path
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = kInt64Max;
    opt.timeout = std::chrono::milliseconds(10);
    const uint64_t id = sub.Subscribe(kTopic, sink.callback(), opt).subscription_id;
    for (int i = 0; i < 3; ++i) pub.Publish(kTopic, MakeRow(i, "n"));

    EXPECT_TRUE(WaitForAccounted(sink, 3, std::chrono::seconds(2)))
        << "not every row was accounted for: " << Describe(sink);
    sub.Unsubscribe(id);

    const Totals t = Sum(sink);
    EXPECT_EQ(t.nulls, 0u) << "a null batch was delivered: " << Describe(sink);
    EXPECT_EQ(t.dropped, 0) << Describe(sink);
    EXPECT_EQ(t.rows, 3) << Describe(sink);
    const std::string seen = Describe(sink);  // locks the sink itself: never inside the guard below
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        bool saw_timeout = false;
        for (const auto& d : sink.deliveries) {
            EXPECT_TRUE(d.status.reason == BatchStatus::Reason::kTimeout ||
                        d.status.reason == BatchStatus::Reason::kClosing);
            saw_timeout |= d.status.reason == BatchStatus::Reason::kTimeout;
        }
        EXPECT_TRUE(saw_timeout) << "the rows were not delivered on a deadline: " << seen;
    }
    ExpectXsRunFrom(sink, 0);
}

// (b) A huge max_rows and no timeout: nothing closes the window but the ending, which delivers
// the three rows.
void ExpectHugeMaxRowsWithNoTimeoutDeliversAtClosing() {
    BatchSink sink;
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = kInt64Max;
    opt.timeout = kNoTimeoutValue;
    const uint64_t id = sub.Subscribe(kTopic, sink.callback(), opt).subscription_id;
    for (int i = 0; i < 3; ++i) pub.Publish(kTopic, MakeRow(i, "n"));

    Observe();
    ASSERT_EQ(Count(sink), 0u) << "a batch arrived before the ending: " << Describe(sink);

    sub.Unsubscribe(id);
    ASSERT_EQ(Count(sink), 1u) << Describe(sink);
    const std::string seen = Describe(sink);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        const auto& d = sink.deliveries[0];
        EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
        EXPECT_EQ(d.status.rows_dropped, 0);
        ASSERT_NE(d.batch, nullptr) << "the rows were reported lost: " << seen;
        EXPECT_EQ(d.num_rows, 3);
    }
    ExpectXsRunFrom(sink, 0);
}

// (c) GUARD: a max_rows above the pre-size bound (kMaxPreSizeRows, 65 536 rows, the design's
// number) still fills a window to exactly the number asked for. A fix that clamped max_rows to the
// bound, or let the bound close a window, closes at 65 536 rows and fails here.
void ExpectMaxRowsAboveThePreSizeBoundFillsExactly() {
    constexpr int64_t kAboveTheBound = (int64_t{1} << 17) + 1;  // 131 073
    BatchSink sink;
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = kAboveTheBound;
    opt.timeout = kNoTimeoutValue;
    const uint64_t id = sub.Subscribe(kTopic, sink.callback(), opt).subscription_id;
    for (int64_t i = 0; i < kAboveTheBound + 1; ++i) {
        pub.Publish(kTopic, MakeRow(static_cast<int32_t>(i), "n"));
    }

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(10))) << "no batch arrived";
    ASSERT_EQ(Count(sink), 1u) << "a window closed before max_rows: " << Describe(sink);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        const auto& d = sink.deliveries[0];
        EXPECT_EQ(d.status.reason, BatchStatus::Reason::kRowLimit);
        EXPECT_EQ(d.status.rows_dropped, 0);
        ASSERT_NE(d.batch, nullptr);
        ASSERT_EQ(d.num_rows, kAboveTheBound);
        EXPECT_EQ(XAt(d, 0), 0);
        EXPECT_EQ(XAt(d, kAboveTheBound - 1), static_cast<int32_t>(kAboveTheBound - 1));
    }

    sub.Unsubscribe(id);
    ASSERT_EQ(Count(sink), 2u) << Describe(sink);
    {
        std::lock_guard<std::mutex> lk(sink.mu);
        const auto& d = sink.deliveries[1];
        EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
        EXPECT_EQ(d.status.rows_dropped, 0);
        ASSERT_NE(d.batch, nullptr);
        ASSERT_EQ(d.num_rows, 1);
        EXPECT_EQ(XAt(d, 0), static_cast<int32_t>(kAboveTheBound));
    }
}

}  // namespace

// Rung 4 (L15, L8; ruling 2026-11-12 "Fix in D7O"). A max_rows larger than any window will reach is
// used as given: the batcher's up-front sizing never makes a decodable schema unusable and never
// lets the number size an allocation.
//  (a) INT64_MAX rows, 10 ms timeout: the 3 rows arrive on a deadline, none lost.
//  (b) INT64_MAX rows, no timeout: nothing within an observation window, then Unsubscribe
//      delivers the 3 rows as the closing batch.
//  (c) GUARD, green both ways: 2^17 + 1 rows (above the pre-size bound) fills to exactly that
//      number, then the closing batch holds the one row after it.
// Red on `957a514` for (a) and (b): the pre-size of INT64_MAX rows fails in SetSchema, the decoder
// is discarded, and every row is reported lost in a null batch (rows_dropped 3). Arm (a) is (b)'s
// control: the same body with a finite timeout does get a timed delivery.
TEST(SubscriberArrowBatchTest, MaxRowsLargerThanAnyWindowIsUsedAsGiven) {
    {
        SCOPED_TRACE("(a) INT64_MAX rows, 10 ms timeout");
        ExpectHugeMaxRowsWithTimeoutDelivers();
    }
    {
        SCOPED_TRACE("(b) INT64_MAX rows, no timeout");
        ExpectHugeMaxRowsWithNoTimeoutDeliversAtClosing();
    }
    {
        SCOPED_TRACE("(c) 2^17 + 1 rows, guard");
        ExpectMaxRowsAboveThePreSizeBoundFillsExactly();
    }
}

// GUARD (DEBT-3 of the design review). The verdict on a schema is the decoder constructor's alone;
// the up-front sizing is a hint. A schema whose rows are as wide as Arrow allows
// (fixed_size_binary of 2^31 - 1 bytes) is decodable, but room for even 65 536 of its rows cannot
// be set aside. The batcher must still hold a decoder: the one malformed row is counted lost in a
// non-null, zero-row closing batch, not in a null one (which is what "no usable schema" reports).
// max_rows is at the pre-size bound or above, so the up-front sizing fails fast on the old tree and
// the new one alike. Red on `957a514`: a null kClosing batch.
TEST(SubscriberArrowBatchTest, AFailedPreSizeDoesNotMakeTheSchemaUnusable) {
    constexpr int64_t kAtLeastTheBound = 100'000;
    BatchSink sink;
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto widest = arrow::schema({arrow::field("w", arrow::fixed_size_binary(INT32_MAX))});
    pub.CreateTopic(kTopic, widest);

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = kAtLeastTheBound;
    opt.timeout = kNoTimeoutValue;
    const uint64_t id = sub.Subscribe(kTopic, sink.callback(), opt).subscription_id;
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });

    sub.Unsubscribe(id);
    ASSERT_EQ(Count(sink), 1u) << Describe(sink);
    std::lock_guard<std::mutex> lk(sink.mu);
    const auto& d = sink.deliveries[0];
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(d.status.rows_dropped, 1);
    ASSERT_NE(d.batch, nullptr) << "the schema was judged unusable";
    EXPECT_EQ(d.num_rows, 0);
}

namespace {

// A sink that stamps every delivery, for the sweep's L5 floor check.
struct TimedSink {
    struct Stamped {
        BatchStatus status;
        int64_t rows;
        bool null;
        std::chrono::steady_clock::time_point at;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Stamped> got;

    SubscriberArrow::RecordBatchCallback callback() {
        return [this](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments>,
                      BatchStatus status) {
            std::lock_guard<std::mutex> lk(mu);
            got.push_back({status, batch ? batch->num_rows() : 0, !batch,
                           std::chrono::steady_clock::now()});
            cv.notify_all();
        };
    }
    bool WaitForAny(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return !got.empty(); });
    }
};

struct SweepCell {
    std::shared_ptr<MockProvider> mock = std::make_shared<MockProvider>();
    PublisherArrow pub{mock};
    SubscriberArrow sub{mock};
};

}  // namespace

// Rung "tests other than rungs": the BC-8 sweep (GUARD). The grid is max_rows x timeout, 6 x 7 = 42
// cells (milliseconds::max() is kNoTimeout, one column). The oracle is L15 written literally:
// a cell is refused exactly when max_rows < 1 || timeout < 0.
//  - Refused (27): kInvalidArgument, the callback released before the exception reaches the caller,
//    nothing reached the provider, and nothing is ever delivered (one observation window, one
//    Publish per refused cell).
//  - Accepted (15): no throw, the provider saw one subscription, and one row published then the
//    subscription ended is delivered whole: sum rows 1, sum dropped 0, no null batch (L15 "used as
//    given"); max_rows 1 closes on the row limit; a timeout of 100 years or more never fires; a
//    timeout that fires does so at least 1 ms after the row arrived (L5 floor).
// Declared overlap with rung 4: the 5 accepted INT64_MAX cells are red on `957a514` for rung 4's
// reason (a null batch, rows_dropped 1). Every other cell is green both ways.
TEST(SubscriberArrowBatchTest, EveryBatchOptionIsRefusedExactlyWhenOutOfRange) {
    using ms = std::chrono::milliseconds;
    struct Rows {
        const char* name;
        int64_t value;
    };
    struct Timeout {
        const char* name;
        ms value;
    };
    const Rows row_counts[] = {{"INT64_MIN", std::numeric_limits<int64_t>::min()},
                               {"-1", -1},
                               {"0", 0},
                               {"1", 1},
                               {"2", 2},
                               {"INT64_MAX", kInt64Max}};
    const Timeout timeouts[] = {
        {"milliseconds::min()", ms::min()},
        {"-1 ms", ms(-1)},
        {"0", ms(0)},
        {"1 ms", ms(1)},
        {"kNoTimeout (milliseconds::max())", kNoTimeoutValue},
        {"100 years", ms(3'155'760'000'000LL)},
        {"the clock's own maximum, in ms",
         std::chrono::duration_cast<ms>(std::chrono::steady_clock::duration::max())},
    };
    constexpr ms kHundredYears(3'155'760'000'000LL);

    std::deque<BatchSink> refused_sinks;  // declared first: outlive every subscriber
    std::deque<SweepCell> refused_cells;
    int cells = 0;
    int refused = 0;
    int accepted = 0;

    for (const Rows& r : row_counts) {
        for (const Timeout& t : timeouts) {
            ++cells;
            SCOPED_TRACE(std::string("max_rows ") + r.name + ", timeout " + t.name);
            const bool expect_refused = r.value < 1 || t.value < ms(0);

            SubscriberArrow::BatchOptions opt;
            opt.max_rows = r.value;
            opt.timeout = t.value;

            if (expect_refused) {
                ++refused;
                refused_sinks.emplace_back();
                refused_cells.emplace_back();
                BatchSink& sink = refused_sinks.back();
                SweepCell& cell = refused_cells.back();
                cell.pub.CreateTopic(kTopic, TestSchema());

                std::weak_ptr<int> token;
                SubscriberArrow::RecordBatchCallback cb;
                {
                    auto strong = std::make_shared<int>(0);
                    token = strong;
                    auto inner = sink.callback();
                    cb = [strong, inner](std::shared_ptr<arrow::RecordBatch> batch,
                                         std::vector<Attachments> att, BatchStatus status) {
                        inner(std::move(batch), std::move(att), status);
                    };
                }
                bool threw = false;
                try {
                    const auto result = cell.sub.Subscribe(kTopic, std::move(cb), opt);
                    ADD_FAILURE() << "out-of-range options were accepted";
                    cell.sub.Unsubscribe(result.subscription_id);
                } catch (const PubSubError& e) {
                    threw = true;
                    EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
                }
                EXPECT_TRUE(threw);
                EXPECT_TRUE(token.expired()) << "a callback is still held after the call returned";
                EXPECT_EQ(cell.mock->subscribe_with_options_count, 0);
                cell.pub.Publish(kTopic, MakeRow(7, "n"));
                continue;
            }

            ++accepted;
            TimedSink sink;  // declared before the cell, so it outlives the subscriber
            SweepCell cell;
            cell.pub.CreateTopic(kTopic, TestSchema());
            uint64_t id = 0;
            try {
                id = cell.sub.Subscribe(kTopic, sink.callback(), opt).subscription_id;
            } catch (const PubSubError& e) {
                ADD_FAILURE() << "in-range options were refused: " << e.what();
                continue;
            }
            EXPECT_EQ(cell.mock->subscribe_with_options_count, 1);

            const auto t0 = std::chrono::steady_clock::now();
            cell.pub.Publish(kTopic, MakeRow(7, "n"));
            // Where a window closes within the test's reach, wait for it, so the timer and the row
            // limit are exercised rather than pre-empted by the Unsubscribe.
            if (r.value == 1 || t.value <= ms(1)) {
                EXPECT_TRUE(sink.WaitForAny(std::chrono::seconds(2)));
            }
            cell.sub.Unsubscribe(id);

            std::lock_guard<std::mutex> lk(sink.mu);
            int64_t rows = 0;
            int64_t dropped = 0;
            for (const auto& g : sink.got) {
                EXPECT_FALSE(g.null) << "a null batch: the row was reported lost";
                rows += g.rows;
                dropped += g.status.rows_dropped;
                if (g.status.reason == BatchStatus::Reason::kTimeout) {
                    EXPECT_LT(t.value, kHundredYears)
                        << "a window closed on a deadline too far to reach";
                    EXPECT_GE(g.at - t0, ms(1)) << "a deadline passed before the 1 ms floor (L5)";
                }
            }
            EXPECT_EQ(rows, 1);
            EXPECT_EQ(dropped, 0);
            if (r.value == 1 && !sink.got.empty()) {
                EXPECT_EQ(sink.got[0].status.reason, BatchStatus::Reason::kRowLimit);
            }
        }
    }
    EXPECT_EQ(cells, 42);
    EXPECT_EQ(refused, 27);
    EXPECT_EQ(accepted, 15);

    // The refused cells received nothing.
    Observe();
    for (size_t i = 0; i < refused_sinks.size(); ++i) {
        std::lock_guard<std::mutex> lk(refused_sinks[i].mu);
        EXPECT_TRUE(refused_sinks[i].deliveries.empty())
            << "refused cell " << i << " received " << refused_sinks[i].deliveries.size()
            << " deliveries";
    }
}

// ---------------------------------------------------------------------------
// D9S-5b -- every close ends with exactly one closing delivery; a refused Subscribe delivers
// nothing (d7 L6, L7, L9 for closing; L1's refused clause; rulings 2026-11-14)
// ---------------------------------------------------------------------------

namespace {

enum class CloseBy { kUnsubscribe, kDestroy };
enum class CloseFrom { kOutside, kInsideOwnCallback, kRowDelivery };
// What the window holds when it is closed: nothing, after a decoded row (a decoder exists);
// nothing, after a row that could not be decoded (no decoder); nothing, before any row (no
// decoder).
enum class IdleState { kDecoded, kUndecoded, kNoRow };

struct CloseCase {
    CloseBy by;
    CloseFrom from;
    IdleState state;
};

std::string CaseName(const CloseCase& c) {
    return std::string(c.by == CloseBy::kDestroy ? "~SubscriberArrow" : "Unsubscribe") +
           (c.from == CloseFrom::kOutside             ? " from outside"
            : c.from == CloseFrom::kInsideOwnCallback ? " inside its own callback"
                                                      : " from a row delivery (D13)") +
           (c.state == IdleState::kDecoded     ? ", idle after a decoded row"
            : c.state == IdleState::kUndecoded ? ", idle after an undecodable row"
                                               : ", before any row");
}

// One cell's state, shared by value with every callback, so a delivery that came too late can
// never reach a destroyed frame: the shared observation window reports it instead.
struct CloseCellState {
    std::unique_ptr<SubscriberArrow> sub;
    std::atomic<uint64_t> id{0};
    std::atomic<bool> fired{false};
    std::atomic<size_t> at_return{std::numeric_limits<size_t>::max()};
    Latch closer_returned;

    void Close(const CloseCase& c, DeliveryLog& log) {
        if (c.by == CloseBy::kDestroy) {
            sub.reset();
        } else {
            sub->Unsubscribe(id.load());
        }
        at_return = log.Count();
        closer_returned.Open();
    }
};

// One cell on its own in-process provider. The trigger is one row at max_rows 1 (decoded: a
// kRowLimit window), or one raw row on an undeclared topic with a 10 ms timeout (undecodable: a
// null kTimeout report), or nothing. Then the window is idle and the subscription is closed. It
// must end with exactly one kClosing, last, 0 dropped: zero rows of the topic's schema with a
// decoder, null without (L7). From outside the closing call returns after it (L9); from inside its
// own callback or from a row delivery it returns first, and the closing follows on the batch thread
// (D12, D13). A closing in the row-delivery cell is held until that call has returned, so a call
// that waited for it is seen at once, not inferred from a timing.
void RunEveryCloseCell(const CloseCase& c, DeliveryLog& log) {
    auto st = std::make_shared<CloseCellState>();
    if (c.from == CloseFrom::kInsideOwnCallback) {
        log.hook = [st, c, &log](size_t index, const DeliveryLog::Rec&) {
            if (index == 0) st->Close(c, log);
        };
    }
    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    if (c.state != IdleState::kUndecoded) pub.CreateTopic(kTopic, TestSchema());
    if (c.from == CloseFrom::kRowDelivery) pub.CreateTopic(kOtherTopic, TestSchema());
    st->sub = std::make_unique<SubscriberArrow>(provider);
    SubscriberArrow::BatchOptions opt = RowLimitOptions(1);
    if (c.state == IdleState::kUndecoded) {
        opt.max_rows = 100;
        opt.timeout = std::chrono::milliseconds(10);
    }
    const bool hold = c.from == CloseFrom::kRowDelivery;
    st->id = st->sub
                 ->Subscribe(
                     kTopic,
                     [st, hold, &log](std::shared_ptr<arrow::RecordBatch> batch,
                                      std::vector<Attachments>, BatchStatus status) {
                         if (hold && status.reason == BatchStatus::Reason::kClosing) {
                             static_cast<void>(st->closer_returned.WaitFor(kAwait));
                         }
                         log.OnDelivery(batch, status);
                     },
                     opt)
                 .subscription_id;
    if (c.from == CloseFrom::kRowDelivery) {
        static_cast<void>(
            st->sub->Subscribe(kOtherTopic, [st, c, &log](ArrowRow, const Attachments&) {
                if (!st->fired.exchange(true)) st->Close(c, log);
            }));
    }

    const size_t before = c.state == IdleState::kNoRow ? 0 : 1;  // deliveries before the closing
    if (c.state == IdleState::kDecoded) pub.Publish(kTopic, MakeRow(0, "n"));
    if (c.state == IdleState::kUndecoded) {
        pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });
    }
    if (c.from != CloseFrom::kInsideOwnCallback) {
        EXPECT_TRUE(log.WaitFor(before, kAwait)) << "the trigger was not delivered";
        EXPECT_TRUE(log.WaitAllReturned(kAwait));
        if (c.from == CloseFrom::kOutside) {
            st->Close(c, log);
        } else {
            pub.Publish(kOtherTopic, MakeRow(-1, "trigger"));
        }
    }
    EXPECT_TRUE(st->closer_returned.WaitFor(kAwait)) << "the closing call did not return";
    EXPECT_TRUE(log.WaitFor(before + 1, kAwait)) << "no closing delivery arrived";
    EXPECT_TRUE(log.WaitAllReturned(kAwait));
    if (st->sub) st->sub.reset();

    EXPECT_EQ(st->at_return.load(), c.from == CloseFrom::kOutside ? before + 1 : before)
        << (c.from == CloseFrom::kOutside ? "the closing call returned before the closing (L9)"
                                          : "the closing call waited for the closing (D12, D13)");
    const auto recs = log.Snapshot();
    ASSERT_EQ(recs.size(), before + 1) << "the trigger, then exactly one closing delivery";
    if (c.state == IdleState::kDecoded) {
        EXPECT_EQ(recs[0].reason, BatchStatus::Reason::kRowLimit);
        EXPECT_EQ(recs[0].num_rows, 1);
    } else if (c.state == IdleState::kUndecoded) {
        EXPECT_EQ(recs[0].reason, BatchStatus::Reason::kTimeout);
        EXPECT_EQ(recs[0].num_rows, -1);
        EXPECT_EQ(recs[0].dropped, 1);
    }
    const auto& closing = recs.back();
    EXPECT_EQ(closing.reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(closing.dropped, 0);
    if (c.state == IdleState::kDecoded) {
        EXPECT_EQ(closing.num_rows, 0) << "with a decoder an empty closing is zero rows (L7)";
        ASSERT_NE(closing.schema, nullptr);
        EXPECT_TRUE(closing.schema->Equals(*TestSchema())) << closing.schema->ToString();
    } else {
        EXPECT_EQ(closing.num_rows, -1) << "without a decoder an empty closing is null (L7)";
    }
    for (const auto& r : recs) {
        EXPECT_EQ(r.thread, recs[0].thread) << "a second callback thread (L12)";
        EXPECT_NE(r.thread, std::this_thread::get_id()) << "a callback on the test thread (D10)";
    }
}

}  // namespace

// Leg (d7 L6 and L9 as amended for closing; L7; D12, D13; ruling 2026-11-14 "every close ends with
// one closing delivery"). F1's cells at d7's own surface: each closer, from outside, from inside
// the subscription's own callback, and (Unsubscribe only: a destroy there is forbidden, 2026-09-05)
// from a row delivery on the same transport, on an idle window after a decoded row, after an
// undecodable one, and before any row. One shared observation window at the end: no cell gets a
// delivery after its closing.
TEST(SubscriberArrowBatchTest, EveryCloseEndsWithExactlyOneClosingDelivery) {
    std::deque<DeliveryLog> logs;  // declared first: every log outlives its cell
    std::vector<CloseCase> cases;
    for (const CloseBy by : {CloseBy::kUnsubscribe, CloseBy::kDestroy}) {
        for (const IdleState s : {IdleState::kDecoded, IdleState::kUndecoded, IdleState::kNoRow}) {
            cases.push_back({by, CloseFrom::kOutside, s});
            if (s != IdleState::kNoRow) cases.push_back({by, CloseFrom::kInsideOwnCallback, s});
            if (by == CloseBy::kUnsubscribe) cases.push_back({by, CloseFrom::kRowDelivery, s});
        }
    }
    std::vector<size_t> counts;
    for (const CloseCase& c : cases) {
        SCOPED_TRACE(CaseName(c));
        logs.emplace_back();
        RunEveryCloseCell(c, logs.back());
        counts.push_back(logs.back().Count());
    }
    Observe();
    for (size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(logs[i].Count(), counts[i])
            << CaseName(cases[i]) << ": a delivery after the closing one (L9)";
    }
}

namespace {

// P6: a transport that delivers rows from inside its own Subscribe and then fails it. Domain 3 has
// already published the new subscription (subscriber.cpp, Subscribe), so the rows reach the batched
// subscription's batcher, and then the Subscribe is refused with the transport's error.
class DeliverThenFailProvider : public PubSubProvider {
   public:
    explicit DeliverThenFailProvider(int rows) : rows_(rows) {}

    void CreateTopic(const std::vector<std::string>&, OwnedSchema schema) override {
        schema_ = std::move(schema);
    }
    void Publish(const std::vector<std::string>&, const RowEncoder&, const Attachments&) override {}
    SubscriptionResult Subscribe(const std::vector<std::string>&,
                                 SubscribeCallback callback) override {
        const Codec codec(TestSchema());
        for (int i = 0; i < rows_; ++i) {
            const EncodedRow row = codec.EncodeRow(MakeRow(i, "caught"));
            callback(row.data(), row.size(), MakeSharedSchema(OwnedSchema::DeepCopy(schema_.get())),
                     Attachments{});
            ++delivered_;
        }
        throw PubSubError(PubSubStatus::kInternal,
                          "DeliverThenFailProvider: the transport failed after delivering");
    }
    void Unsubscribe(const std::vector<std::string>&) override {}

    int delivered() const { return delivered_; }

   private:
    const int rows_;
    int delivered_ = 0;
    OwnedSchema schema_;
};

}  // namespace

// Leg, deterministic (d7 L1's refused clause; ruling 2026-11-14 "a refused batched subscribe never
// delivers"). The transport delivers three rows inside Subscribe, then fails it. With max_rows 1
// those rows were queued as full windows; with max_rows 100 they were pending. Either way the
// failure is rethrown as the refusal, the callback is never called, and it has been released when
// the call returns. Nothing is waited for: a refusal from outside a delivery returns only once the
// batch thread has ended, so the counter is final when it is read.
TEST(SubscriberArrowBatchTest, ARefusedSubscribeNeverDeliversWhatItCaught) {
    constexpr int kCaught = 3;
    for (const int64_t max_rows : {int64_t{1}, int64_t{100}}) {
        SCOPED_TRACE(max_rows == 1 ? "max_rows 1: full windows were queued"
                                   : "max_rows 100: rows were pending");
        auto provider = std::make_shared<DeliverThenFailProvider>(kCaught);
        PublisherArrow pub(provider);
        pub.CreateTopic(kTopic, TestSchema());
        SubscriberArrow sub(provider);

        const auto calls = std::make_shared<std::atomic<int>>(0);
        std::weak_ptr<int> token;
        SubscriberArrow::RecordBatchCallback cb;
        {
            auto strong = std::make_shared<int>(0);
            token = strong;
            cb = [strong, calls](std::shared_ptr<arrow::RecordBatch>, std::vector<Attachments>,
                                 BatchStatus) { ++*calls; };
        }
        bool refused = false;
        try {
            static_cast<void>(sub.Subscribe(kTopic, std::move(cb), RowLimitOptions(max_rows)));
            ADD_FAILURE() << "the Subscribe was not refused";
        } catch (const PubSubError& e) {
            refused = e.status() == PubSubStatus::kInternal;
        }
        EXPECT_TRUE(refused) << "the transport's failure did not reach the caller as the refusal";
        EXPECT_EQ(provider->delivered(), kCaught) << "precondition: the transport delivered";
        EXPECT_EQ(calls->load(), 0) << "the refused Subscribe delivered what it caught";
        EXPECT_TRUE(token.expired()) << "the refused Subscribe still holds its callback";
    }
}
