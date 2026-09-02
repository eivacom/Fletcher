// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <mutex>
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
        std::string key = Join(segments);
        topics_created.push_back(key);
        if (schema) {
            schemas_[key] = OwnedSchema::DeepCopy(schema.get());
        }
    }

    void Publish(const std::vector<std::string>& segments, const RowEncoder& encoder,
                 const Attachments& attachments) override {
        std::string key = Join(segments);

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
        std::string key = Join(segments);
        callbacks_[key] = std::move(callback);
        auto it = schemas_.find(key);
        SharedSchema schema;
        if (it != schemas_.end()) {
            schema = MakeSharedSchema(OwnedSchema::DeepCopy(it->second.get()));
        }
        return {MakeReadySchemaFuture(std::move(schema))};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
    }

    std::vector<std::string> topics_created;
    // Raw bytes handed to the RowEncoder on each underlying Publish() call, one entry per call.
    std::vector<std::vector<uint8_t>> published;

   private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::unordered_map<std::string, OwnedSchema> schemas_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) {
                out += '/';
            }
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

TEST(PublisherArrowTest, PublishTypeMismatchThrowsToCaller) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    pub.CreateTopic(kTopic, arrow::schema({arrow::field("a", arrow::int32())}));

    ArrowRow row = {std::make_shared<arrow::StringScalar>("nope")};
    EXPECT_THROW(pub.Publish(kTopic, row), std::invalid_argument);

    EXPECT_TRUE(mock->published.empty());
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

// ---------------------------------------------------------------------------
// SubscriberArrow tests
// ---------------------------------------------------------------------------

TEST(SubscriberArrowTest, SubscribeReturnsArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());
    SubscriberArrow::SubscribeResult result = sub.Subscribe(kTopic, [](ArrowRow, Attachments) {});

    std::shared_ptr<arrow::Schema> sch = result.schema.get();
    ASSERT_NE(sch, nullptr);
    EXPECT_EQ(sch->num_fields(), 2);
    EXPECT_EQ(sch->field(0)->name(), "x");
    EXPECT_TRUE(sch->field(0)->type()->Equals(*arrow::int32()));
    EXPECT_EQ(sch->field(1)->name(), "name");
    EXPECT_TRUE(sch->field(1)->type()->Equals(*arrow::utf8()));
}

TEST(PubSubArrowTest, PublishSubscribeRoundtripWithArrowRow) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    ArrowRow received;
    sub.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

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
    sub.Subscribe(kTopic, [&](ArrowRow, const Attachments& att) { received_att = att; });

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD});

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pub.Publish(kTopic, row, {{"img", blob}});

    ASSERT_EQ(received_att.count("img"), 1);
    EXPECT_EQ(*received_att.at("img"), *blob);
}

TEST(PubSubArrowTest, PublishDirectPassthrough) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    std::shared_ptr<arrow::Schema> schema = arrow::schema({arrow::field("x", arrow::int32())});
    pub.CreateTopic(kTopic, schema);

    ArrowRow received;
    sub.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

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

// Thread-safe sink for delivered batches (batches may arrive on the batcher's
// timer thread for timeout flushes).
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

TEST(SubscriberArrowBatchTest, FlushesAtRowLimit) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);
    pub.CreateTopic(kTopic, TestSchema());

    BatchSink sink;
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 3;
    opt.timeout = std::chrono::minutes(10);  // long, so only the count triggers
    sub.Subscribe(kTopic, sink.callback(), opt);

    for (int i = 0; i < 3; ++i) pub.Publish(kTopic, MakeRow(i, "n"));

    // The count trigger flushes synchronously on the publishing thread.
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
    sub.Subscribe(kTopic, sink.callback(), opt);

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
    sub.Subscribe(kTopic, sink.callback(), opt);

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xBE, 0xEF});
    pub.Publish(kTopic, MakeRow(1, "a"), {{"img", blob}});  // row 0 has an attachment
    pub.Publish(kTopic, MakeRow(2, "b"));                   // row 1 has none

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    ASSERT_EQ(sink.deliveries[0].num_rows, 2);
    ASSERT_EQ(sink.deliveries[0].attachments.size(), 2u);
    EXPECT_EQ(sink.deliveries[0].attachments[0].count("img"), 1u);
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

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x01});
    pub.Publish(kTopic, MakeRow(1, "good"), {{"img", blob}});  // decodes fine

    // A truncated buffer (just the 1-byte null bitfield) underruns when the
    // int32 field is read, so DecodeRow throws and the row is dropped — and
    // its attachment is discarded with it.
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) { buf.AppendByte(0x00); }, {{"orphan", blob}});

    sub.Unsubscribe(result.subscription_id);

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    EXPECT_EQ(sink.deliveries[0].num_rows, 1);             // only the good row
    EXPECT_EQ(sink.deliveries[0].attachments.size(), 1u);  // dropped row's attachment gone
    EXPECT_EQ(sink.deliveries[0].attachments[0].count("img"), 1u);
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
    sub.Subscribe(kTopic, sink.callback(), opt);

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
    sub.Subscribe(kTopic, sink.callback(), opt);

    // Published as plain values; subscriber re-folds into a dictionary.
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("red")});
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("blue")});
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("red")});

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
    sub.Subscribe(kTopic, sink.callback(), opt);

    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("x")});
    pub.Publish(kTopic, {arrow::MakeNullScalar(arrow::utf8())});
    pub.Publish(kTopic, {std::make_shared<arrow::StringScalar>("x")});

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
// BatchDecoder-backed batching (Step 4) — corrupt rows, capacity, reuse
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
    sub.Subscribe(kTopic, sink.callback(), opt);

    arrow::StringBuilder sb_a;
    ASSERT_TRUE(sb_a.AppendValues({"a", "b"}).ok());
    ArrowRow row_a = {std::make_shared<arrow::ListScalar>(sb_a.Finish().ValueOrDie()),
                      std::make_shared<arrow::Int32Scalar>(1)};
    arrow::StringBuilder sb_b;
    ASSERT_TRUE(sb_b.AppendValues({"c"}).ok());
    ArrowRow row_b = {std::make_shared<arrow::ListScalar>(sb_b.Finish().ValueOrDie()),
                      std::make_shared<arrow::Int32Scalar>(2)};

    auto blob_a = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xAA});
    auto blob_b = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xBB});

    pub.Publish(kTopic, row_a, {{"blob", blob_a}});

    // A corrupt row: row A's own encoding, truncated by 3 bytes so the fixed 4-byte
    // int32 field underruns on read — BatchDecoder throws, nothing is appended.
    Codec codec(schema);
    EncodedRow encoded_a = codec.EncodeRow(row_a);
    ASSERT_GT(encoded_a.size(), 3u);
    EncodedRow truncated(encoded_a.begin(), encoded_a.end() - 3);
    pub.PublishDirect(kTopic,
                      [&](WriteBuffer& buf) { buf.Append(truncated.data(), truncated.size()); });

    pub.Publish(kTopic, row_b, {{"blob", blob_b}});  // reaches max_rows -> synchronous flush

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    const auto& d = sink.deliveries[0];
    EXPECT_EQ(d.num_rows, 2);
    EXPECT_EQ(d.status.rows_dropped, 1);
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kRowLimit);
    ASSERT_EQ(d.attachments.size(), 2u);
    EXPECT_EQ(*d.attachments[0].at("blob"), *blob_a);
    EXPECT_EQ(*d.attachments[1].at("blob"), *blob_b);

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
    sub.Subscribe(kTopic, sink.callback(), opt);

    arrow::FloatBuilder fb;
    ASSERT_TRUE(fb.AppendValues({1.0f, 2.0f, 3.0f}).ok());
    auto fsl_scalar =
        std::make_shared<arrow::FixedSizeListScalar>(fb.Finish().ValueOrDie(), fsl_type);
    pub.Publish(kTopic, {fsl_scalar});

    std::lock_guard<std::mutex> lk(sink.mu);
    ASSERT_EQ(sink.deliveries.size(), 1u);
    auto batch = sink.deliveries[0].batch;
    ASSERT_NE(batch, nullptr);
    ASSERT_EQ(batch->num_rows(), 1);
    auto col = batch->column(0);
    ASSERT_TRUE(col->IsValid(0));  // this was silently nulled before Step 1's F6 fix + this step
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
    sub.Subscribe(kTopic, sink.callback(), opt);

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
