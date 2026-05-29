// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub_arrow/pubsub_arrow.hpp>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Mock provider (nanoarrow interface)
// ---------------------------------------------------------------------------

class MockProvider : public PubSub {
   public:
    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema,
                     std::any /*config*/) override {
        std::string key = Join(segments);
        topics_created.push_back(key);
        if (schema) schemas_[key] = OwnedSchema::DeepCopy(schema.get());
    }

    void Publish(const std::vector<std::string>& segments, RowEncoder encoder,
                 const Attachments& attachments) override {
        std::string key = Join(segments);

        std::vector<uint8_t> buf;
        VectorWriteBuffer wb(buf);
        encoder(wb);

        auto it = callbacks_.find(key);
        if (it != callbacks_.end()) {
            SharedSchema sp;
            auto sit = schemas_.find(key);
            if (sit != schemas_.end())
                sp = MakeSharedSchema(OwnedSchema::DeepCopy(sit->second.get()));
            it->second(buf.data(), buf.size(), sp, attachments);
        }
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback, std::any /*config*/) override {
        std::string key = Join(segments);
        callbacks_[key] = std::move(callback);
        auto it = schemas_.find(key);
        OwnedSchema schema;
        if (it != schemas_.end()) schema = OwnedSchema::DeepCopy(it->second.get());
        return {std::move(schema)};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
    }

    std::vector<std::string> topics_created;

   private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::unordered_map<std::string, OwnedSchema> schemas_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) out += '/';
            out += segs[i];
        }
        return out;
    }
};

static auto TestSchema() {
    return arrow::schema({
        arrow::field("x", arrow::int32()),
        arrow::field("name", arrow::utf8()),
    });
}

static const std::vector<std::string> kTopic = {"test", "topic"};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(PubSubArrowTest, CreateTopicConvertsArrowSchemaToArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    pa.CreateTopic(kTopic, TestSchema());
    ASSERT_EQ(mock->topics_created.size(), 1);
    EXPECT_EQ(mock->topics_created[0], "test/topic");
}

TEST(PubSubArrowTest, SubscribeReturnsArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    pa.CreateTopic(kTopic, TestSchema());
    auto result = pa.Subscribe(kTopic, [](ArrowRow, Attachments) {});

    ASSERT_NE(result.schema, nullptr);
    EXPECT_EQ(result.schema->num_fields(), 2);
    EXPECT_EQ(result.schema->field(0)->name(), "x");
    EXPECT_TRUE(result.schema->field(0)->type()->Equals(*arrow::int32()));
    EXPECT_EQ(result.schema->field(1)->name(), "name");
    EXPECT_TRUE(result.schema->field(1)->type()->Equals(*arrow::utf8()));
}

TEST(PubSubArrowTest, PublishSubscribeRoundtripWithArrowRow) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    pa.CreateTopic(kTopic, TestSchema());

    ArrowRow received;
    pa.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

    ArrowRow sent = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
    };
    pa.Publish(kTopic, sent);

    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*received[0]).value, 42);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*received[1]).value->ToString(), "hello");
}

TEST(PubSubArrowTest, PublishWithAttachments) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    pa.CreateTopic(kTopic, TestSchema());

    Attachments received_att;
    pa.Subscribe(kTopic, [&](ArrowRow, Attachments att) { received_att = std::move(att); });

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD});

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pa.Publish(kTopic, row, {{"img", blob}});

    ASSERT_EQ(received_att.count("img"), 1);
    EXPECT_EQ(*received_att.at("img"), *blob);
}

TEST(PubSubArrowTest, PublishDirectPassthrough) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    auto schema = arrow::schema({arrow::field("x", arrow::int32())});
    pa.CreateTopic(kTopic, schema);

    ArrowRow received;
    pa.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

    // Publish using direct encoder (positional format: 1 field).
    pa.PublishDirect(kTopic, [](WriteBuffer& buf) {
        buf.AppendByte(0x00);  // null bitfield: 1 field, not null
        int32_t val = 99;
        buf.Append(reinterpret_cast<const uint8_t*>(&val), sizeof(val));
    });

    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*received[0]).value, 99);
}

TEST(PubSubArrowTest, Unsubscribe) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    pa.CreateTopic(kTopic, TestSchema());

    int count = 0;
    auto result = pa.Subscribe(kTopic, [&](ArrowRow, Attachments) { count++; });

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pa.Publish(kTopic, row);
    EXPECT_EQ(count, 1);

    pa.Unsubscribe(result.subscription_id);
    pa.Publish(kTopic, row);
    EXPECT_EQ(count, 1);  // not incremented
}

TEST(PubSubArrowTest, ListTopicsAndHasTopic) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);

    EXPECT_TRUE(pa.ListTopics().empty());
    EXPECT_FALSE(pa.HasTopic(kTopic));

    pa.CreateTopic(kTopic, TestSchema());
    EXPECT_TRUE(pa.HasTopic(kTopic));

    auto topics = pa.ListTopics();
    ASSERT_EQ(topics.size(), 1);
    EXPECT_EQ(topics[0], "test/topic");
}

// ---------------------------------------------------------------------------
// Batched (RecordBatch) Subscribe
// ---------------------------------------------------------------------------

namespace {

using BatchStatus = PubSubArrow::BatchStatus;

// Thread-safe sink for delivered batches (batches may arrive on the batcher's
// timer thread for timeout flushes).
struct BatchSink {
    struct Delivery {
        int64_t num_rows;
        std::vector<Attachments> attachments;
        BatchStatus status;
    };

    std::mutex mu;
    std::condition_variable cv;
    std::vector<Delivery> deliveries;

    PubSubArrow::RecordBatchCallback callback() {
        return [this](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments> att,
                      BatchStatus status) {
            std::lock_guard<std::mutex> lk(mu);
            deliveries.push_back({batch ? batch->num_rows() : -1, std::move(att), status});
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

TEST(PubSubArrowBatchTest, FlushesAtRowLimit) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);
    pa.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    PubSubArrow::BatchOptions opt;
    opt.max_rows = 3;
    opt.timeout = std::chrono::minutes(10);  // long, so only the count triggers
    pa.Subscribe(kTopic, sink.callback(), opt);

    for (int i = 0; i < 3; ++i) pa.Publish(kTopic, MakeRow(i, "n"));

    // The count trigger flushes synchronously on the publishing thread.
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 3);
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 3u);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 0);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kRowLimit);
}

TEST(PubSubArrowBatchTest, FlushesAtTimeout) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);
    pa.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    PubSubArrow::BatchOptions opt;
    opt.max_rows = 100000;  // high, so only the timeout triggers
    opt.timeout = std::chrono::milliseconds(100);
    pa.Subscribe(kTopic, sink.callback(), opt);

    pa.Publish(kTopic, MakeRow(1, "a"));
    pa.Publish(kTopic, MakeRow(2, "b"));

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 2);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 0);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kTimeout);
}

TEST(PubSubArrowBatchTest, ClosingFlushOnUnsubscribe) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);
    pa.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    PubSubArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::minutes(10);
    auto result = pa.Subscribe(kTopic, sink.callback(), opt);

    pa.Publish(kTopic, MakeRow(7, "x"));
    pa.Publish(kTopic, MakeRow(8, "y"));
    pa.Unsubscribe(result.subscription_id);  // flushes the partial batch

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 2);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kClosing);
}

TEST(PubSubArrowBatchTest, AttachmentsAlignWithRows) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);
    pa.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    PubSubArrow::BatchOptions opt;
    opt.max_rows = 2;
    opt.timeout = std::chrono::minutes(10);
    pa.Subscribe(kTopic, sink.callback(), opt);

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xBE, 0xEF});
    pa.Publish(kTopic, MakeRow(1, "a"), {{"img", blob}});  // row 0 has an attachment
    pa.Publish(kTopic, MakeRow(2, "b"));                   // row 1 has none

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    ASSERT_EQ(sink.deliveries[0].num_rows, 2);
    ASSERT_EQ(sink.deliveries[0].attachments.size(), 2u);
    EXPECT_EQ(sink.deliveries[0].attachments[0].count("img"), 1u);
    EXPECT_TRUE(sink.deliveries[0].attachments[1].empty());
}

TEST(PubSubArrowBatchTest, DroppedRowReportedAndAttachmentDiscarded) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);
    pa.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    PubSubArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::minutes(10);
    auto result = pa.Subscribe(kTopic, sink.callback(), opt);

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x01});
    pa.Publish(kTopic, MakeRow(1, "good"), {{"img", blob}});  // decodes fine

    // A truncated buffer (just the 1-byte null bitfield) underruns when the
    // int32 field is read, so DecodeRow throws and the row is dropped — and
    // its attachment is discarded with it.
    pa.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); }, {{"orphan", blob}});

    pa.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 1);             // only the good row
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 1u);  // dropped row's attachment gone
    EXPECT_EQ(sink.deliveries[0].attachments[0].count("img"), 1u);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 1);
}

TEST(PubSubArrowBatchTest, OnlyDroppedRowsStillDeliversEmptyBatch) {
    auto mock = std::make_shared<MockProvider>();
    PubSubArrow pa(mock);
    pa.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    PubSubArrow::BatchOptions opt;
    opt.max_rows = 100000;
    opt.timeout = std::chrono::milliseconds(100);
    pa.Subscribe(kTopic, sink.callback(), opt);

    // Only a malformed row arrives in this window.
    pa.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); });

    ASSERT_TRUE(sink.WaitFor(1, std::chrono::seconds(2)));
    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 0);  // zero-row batch (decision A)
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 0u);
    EXPECT_EQ(sink.deliveries[0].status.rows_dropped, 1);
    EXPECT_EQ(sink.deliveries[0].status.reason, BatchStatus::Reason::kTimeout);
}
