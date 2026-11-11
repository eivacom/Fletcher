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
#include <mutex>
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    TopicOptions options{.profile = "reliable"};
    static_cast<void>(
        sub.Subscribe(kTopic, sink.callback(), SubscriberArrow::BatchOptions{}, options));

    EXPECT_EQ(mock->subscribe_with_options_count, 1);
    EXPECT_EQ(mock->last_subscribe_options, options);
}

TEST(SubscriberArrowBatchTest, FlushesAtRowLimit) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
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

// G1 (guard, green before and after): an idle schema-less subscription delivers nothing at
// close. Forbids a null batch that means "no rows" (L7) and any delivery for an idle window (L6).
TEST(SubscriberArrowBatchTest, NoUsableSchemaIdleSubscriptionDeliversNothingAtClose) {
    auto mock = std::make_shared<MockProvider>();
    SubscriberArrow sub(mock);

    BatchSink sink;
    auto result = sub.Subscribe(kTopic, sink.callback(), NoTriggerBatchOptions());
    sub.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    EXPECT_EQ(sink.deliveries.size(), 0u);
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

    sub.Unsubscribe(result.subscription_id);  // both windows were reported; nothing is left
    std::lock_guard<std::mutex> lk(sink.mu);
    EXPECT_EQ(sink.deliveries.size(), after_w2);
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

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), expected_dropped.size());  // exactly these: not none, not more
    for (size_t i = 0; i < expected_dropped.size(); ++i) {
        const auto& d = sink.deliveries[i];
        EXPECT_EQ(d.status.reason, ending == Ending::kDeadline ? BatchStatus::Reason::kTimeout
                                                               : BatchStatus::Reason::kClosing)
            << "delivery " << i;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    pub.CreateTopic(kTopic, arrow::schema({arrow::field("category", dict_type, true)}));

    BatchSink sink;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    pub.CreateTopic(kTopic, arrow::schema({arrow::field("category", dict_type, true)}));

    BatchSink sink;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto schema = arrow::schema({
        arrow::field("tags", arrow::list(arrow::utf8())),
        arrow::field("x", arrow::int32()),
    });
    pub.CreateTopic(kTopic, schema);

    BatchSink sink;
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
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    auto fsl_type = arrow::fixed_size_list(arrow::field("v", arrow::float32(), false), 3);
    auto schema = arrow::schema({arrow::field("vec", fsl_type)});
    pub.CreateTopic(kTopic, schema);

    BatchSink sink;
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
// row is a full window of its own and nothing is pending when the callback unsubscribes: there is
// no closing batch to deliver, and nothing may follow. The callback signals once its Unsubscribe
// has returned; only then are rows 2-3 published, and the count is read after ~SubscriberArrow,
// which waits for every delivery the subscription still owes.
TEST(SubscriberArrowBatchTest, UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    std::atomic<int> delivery_count{0};
    std::atomic<uint64_t> sub_id{0};
    std::promise<void> unsubscribed;
    std::future<void> unsubscribed_future = unsubscribed.get_future();
    auto sub = std::make_unique<SubscriberArrow>(mock);
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 1;  // every published row is a full window of its own
    opt.timeout = std::chrono::minutes(10);
    auto result = sub->Subscribe(
        kTopic,
        [&](std::shared_ptr<arrow::RecordBatch>, std::vector<Attachments>, BatchStatus) {
            if (++delivery_count != 1) return;  // counted; a second delivery fails below
            sub->Unsubscribe(sub_id.load());
            unsubscribed.set_value();
        },
        opt);
    sub_id = result.subscription_id;

    pub.Publish(kTopic, MakeRow(1, "a"));  // a full window -> the callback above
    ASSERT_EQ(unsubscribed_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

    // No further delivery: the subscription is gone, and the process has not crashed getting here.
    pub.Publish(kTopic, MakeRow(2, "b"));
    pub.Publish(kTopic, MakeRow(3, "c"));
    sub.reset();
    EXPECT_EQ(delivery_count.load(), 1);
}

// Pins L11 (RecordBatchBatcher::Start's fix): the callback runs on the subscription's own batch
// thread, so an Unsubscribe from inside it makes that same thread take Stop()'s self-detach
// branch. Before the fix the thread's lambda held only a raw `this`, so the shared_ptr
// Unsubscribe drops there was the last owner and destroyed the batcher while the thread was still
// running inside it (a use-after-free that may or may not crash under a given allocator). With the
// fix the thread owns a shared_ptr of its own, kept alive until its loop returns, so the object
// outlives the thread's use of it.
TEST(SubscriberArrowBatchTest, UnsubscribeFromInsideATimeoutFlushIsSafe) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    std::mutex id_mu;
    uint64_t sub_id = 0;
    std::atomic<bool> unsubscribed{false};
    std::promise<void> delivered;
    std::future<void> delivered_future = delivered.get_future();

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 100;  // never reached; only the timeout triggers the flush
    opt.timeout = std::chrono::milliseconds(20);
    auto result = sub.Subscribe(
        kTopic,
        [&](std::shared_ptr<arrow::RecordBatch>, std::vector<Attachments>, BatchStatus) {
            // The one window was taken by this kTimeout delivery, so nothing is pending and the
            // Unsubscribe below owes no closing batch. A second delivery, nested in that
            // Unsubscribe or after it, is a failure (L9, L12).
            const bool second = unsubscribed.exchange(true);
            EXPECT_FALSE(second) << "a second batch was delivered";
            if (second) return;
            uint64_t id;
            {
                std::lock_guard<std::mutex> lk(id_mu);
                id = sub_id;
            }
            sub.Unsubscribe(id);  // runs on the batch thread -- the path the fix protects
            delivered.set_value();
        },
        opt);
    {
        std::lock_guard<std::mutex> lk(id_mu);
        sub_id = result.subscription_id;  // visible to the callback before the row is published
    }

    pub.Publish(kTopic, MakeRow(1, "a"));

    ASSERT_EQ(delivered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    // sub goes out of scope here; without the fix the batch thread would already have used freed
    // memory well before reaching this point.
}

TEST(SubscriberArrowBatchTest, BatchesAreValidArrow) {
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

    BatchSink sink;
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
// event. Generous: on a correct tree each wait ends within about kLetItWait.
constexpr auto kHold = std::chrono::milliseconds(2000);
constexpr auto kAwait = std::chrono::milliseconds(3000);
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
        Rec rec{status.reason, batch ? batch->num_rows() : -1, -1, -1, status.rows_dropped,
                std::this_thread::get_id()};
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
        first_released_by_publisher = all_published.WaitFor(std::chrono::milliseconds(500));
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
    ExpectDeliveredInOrderOnOneBatchThread(log,
                                           {{BatchStatus::Reason::kRowLimit, 0, 2},
                                            {BatchStatus::Reason::kRowLimit, 2, 2},
                                            {BatchStatus::Reason::kRowLimit, 4, 2}},
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
    EXPECT_TRUE(unsubscribed.WaitFor(2 * kAwait));

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

enum class InsideClosing { kFromTimeout, kFromRowLimit, kWithQueuedWindow, kFromRowDelivery };

// Rung 6. The first batch callback is held while a publisher thread sends rows after it; then the
// subscription is closed from inside that callback (a-c), or from a thread delivering rows on the
// subscription's own transport while that callback is held (d; owner ruling 2026-11-11 (2)). The
// Unsubscribe returns at once with nothing delivered inside it; after the held callback returns,
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
    log.hook = [&](size_t index, const DeliveryLog::Rec&) {
        if (index != 0) return;
        entered.Open();
        if (from_row_delivery) {
            // Held until the row-delivery thread's Unsubscribe has returned.
            released_by_unsubscribe = release.WaitFor(kHold);
        } else {
            static_cast<void>(published.WaitFor(kHold));
            sub->Unsubscribe(id.load());
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
    const bool timeout_first = closing == InsideClosing::kFromTimeout || from_row_delivery;
    SubscriberArrow::BatchOptions opt = RowLimitOptions(2);
    if (timeout_first) {
        opt.max_rows = 100;  // never reached: the first window closes at its deadline
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

    // Rows left pending (k < max_rows) after the held window; (c) first queues a full window.
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
    EXPECT_TRUE(log.WaitFor(closing == InsideClosing::kWithQueuedWindow ? 3 : 2, kAwait));
    EXPECT_TRUE(log.WaitAllReturned(kAwait));
    sub.reset();

    EXPECT_TRUE(published_while_held.load())
        << "rows could not be published while the first callback was held (D10)";
    EXPECT_TRUE(unsubscribe_returned_at_once.load())
        << (from_row_delivery ? "the row-delivery thread's Unsubscribe waited for the held batch "
                                "callback instead of returning at once"
                              : "the in-callback Unsubscribe did not return");
    EXPECT_EQ(at_unsubscribe_return.load(), 1u)
        << "a batch was delivered before the Unsubscribe returned";
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
    }
    std::vector<std::thread::id> foreign = {publisher_id, std::this_thread::get_id()};
    ExpectDeliveredInOrderOnOneBatchThread(log, expected, foreign);
}

}  // namespace

// Rung 6 (L9 as revised, L11, L12; owner rulings "Deliver after it returns", "Queued batch, then
// closing", and 2026-11-11 (2) for case (d)).
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
}

// Guard (green before and after; BC-5). A batch callback unsubscribes, which reaches the in-process
// provider's Unsubscribe, while another thread is inside Publish holding that provider's mutex (a
// per-row delivery on another topic that dwells). Both calls return; the batch callback ran once.
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
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].reason, BatchStatus::Reason::kTimeout);
    EXPECT_EQ(recs[0].num_rows, 1);
}

// Guard (BC-1): fixed seeds; max_rows 1-3, timeout 1 ms; a publisher thread sends rows in bursts;
// the subscription is closed from inside a callback or from outside at a seeded point. Every
// callback runs on one thread that is not a publishing or closing thread, never two at once,
// windows arrive in order (first x strictly increasing), and kClosing, if any, is last.
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
            EXPECT_TRUE(closed.WaitFor(kAwait)) << "no delivery reached x " << close_at;
        } else {
            EXPECT_TRUE(reached.WaitFor(kAwait));
            sub->Unsubscribe(id.load());  // while the publisher keeps publishing
        }
        publisher.join();
        EXPECT_TRUE(log.WaitAllReturned(kAwait));
        sub.reset();

        const auto recs = log.Snapshot();
        EXPECT_LE(log.max_in_flight.load(), 1) << "two batch callbacks ran at once (L12)";
        EXPECT_LE(log.max_depth.load(), 1) << "a batch was delivered inside a batch callback";
        ASSERT_FALSE(recs.empty());
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
            EXPECT_TRUE(closed.WaitFor(kAwait)) << "no delivery after the last Publish";
        } else {
            sub->Unsubscribe(id.load());
        }
        EXPECT_TRUE(log.WaitAllReturned(kAwait));
        sub.reset();

        int64_t accounted = 0;
        for (const auto& r : log.Snapshot()) {
            accounted += std::max<int64_t>(r.num_rows, 0) + r.dropped;
        }
        EXPECT_EQ(accounted, sent) << "rows received + rows reported lost != rows sent";
    }
}
