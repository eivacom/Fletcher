// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// pubsub-arrow + fastdds-pubsub-provider integration test.
//
// Drives the PubSubArrow adapter on top of a real FastDDSPubSubProvider in
// the same process (publisher and subscriber on separate DomainParticipants
// but the same DDS domain) and verifies that ArrowRow instances + their
// schema round-trip across the adapter + DDS path.
//
// Each component has its own unit tests in isolation; only this test
// covers the seam between them.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace fletcher;
using namespace std::chrono_literals;

namespace {

// Use a high domain id to keep this test isolated from any other DDS
// traffic on the host running CI.
constexpr uint32_t kTestDomain = 137;

std::shared_ptr<arrow::Schema> SensorSchema() {
    return arrow::schema({
        arrow::field("sensor_id", arrow::int32(), false),
        arrow::field("temperature", arrow::float64(), false),
        arrow::field("label", arrow::utf8(), false),
    });
}

ArrowRow SensorRow(int32_t id, double temp, const std::string& label) {
    return {
        std::make_shared<arrow::Int32Scalar>(id),
        std::make_shared<arrow::DoubleScalar>(temp),
        std::make_shared<arrow::StringScalar>(label),
    };
}

}  // namespace

TEST(PubSubArrowFastDdsTest, SchemaAndRowDeliveredAcrossDdsBoundary) {
    FastDDSProviderOptions pub_opts;
    pub_opts.domain_id = kTestDomain;
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(pub_opts));
    FastDDSProviderOptions sub_opts;
    sub_opts.domain_id = kTestDomain;
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(sub_opts));

    // Capture state must outlive `sub`: a late DDS callback that fires while
    // the subscriber is tearing down would otherwise touch destroyed locals.
    std::mutex mu;
    std::condition_variable cv;
    bool received = false;
    ArrowRow rx_row;

    PublisherArrow pub(pub_provider);
    SubscriberArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "feed"};

    pub.CreateTopic(topic, schema);

    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        received = true;
        cv.notify_all();
    });

    // Subscribe internally polls until the schema arrives via the companion
    // /__schema topic, so once it returns the subscriber knows the schema.
    std::shared_ptr<arrow::Schema> sub_schema = result.schema.get();
    ASSERT_NE(sub_schema, nullptr);
    EXPECT_TRUE(sub_schema->Equals(*schema, /*check_metadata=*/false));

    // No sleep before publish. The data-topic DataWriter is RELIABLE +
    // TRANSIENT_LOCAL + KEEP_ALL, so the sample is retained and delivered
    // as soon as the DataReader matches. The condition variable below
    // handles the actual wait.
    pub.Publish(topic, SensorRow(42, 23.5, "alpha"));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return received; }))
            << "subscriber callback never fired within 5 s";
    }
    // Unsubscribe before reading rx_row: the test publishes one row, so once
    // the subscription is torn down nothing can mutate rx_row from another
    // thread.
    sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 42);
    EXPECT_EQ(std::static_pointer_cast<arrow::DoubleScalar>(rx_row[1])->value, 23.5);
    EXPECT_EQ(std::static_pointer_cast<arrow::StringScalar>(rx_row[2])->ToString(), "alpha");
}

TEST(PubSubArrowFastDdsTest, MultipleRowsDeliveredInOrder) {
    FastDDSProviderOptions pub_opts;
    pub_opts.domain_id = kTestDomain;
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(pub_opts));
    FastDDSProviderOptions sub_opts;
    sub_opts.domain_id = kTestDomain;
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(sub_opts));

    // Capture state must outlive `sub`: a late DDS callback that fires while
    // the subscriber is tearing down would otherwise touch destroyed locals.
    std::mutex mu;
    std::condition_variable cv;
    std::vector<int32_t> received_ids;

    PublisherArrow pub(pub_provider);
    SubscriberArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "stream"};

    pub.CreateTopic(topic, schema);

    constexpr int kRowCount = 5;

    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        received_ids.push_back(std::static_pointer_cast<arrow::Int32Scalar>(row[0])->value);
        cv.notify_all();
    });

    ASSERT_NE(result.schema.get(), nullptr);

    // No sleep before publish. KEEP_ALL durability retains every sample
    // until the reader matches and consumes them in published order.
    for (int i = 0; i < kRowCount; ++i) {
        pub.Publish(topic, SensorRow(i, static_cast<double>(i), "row-" + std::to_string(i)));
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return received_ids.size() == kRowCount; }))
            << "expected " << kRowCount << " rows, got " << received_ids.size();
    }
    // Unsubscribe before reading received_ids: KEEP_ALL guarantees exactly
    // kRowCount samples were delivered, so once the subscription is torn
    // down nothing further can mutate the vector.
    sub.Unsubscribe(result.subscription_id);

    for (int i = 0; i < kRowCount; ++i) {
        EXPECT_EQ(received_ids[i], i) << "row " << i << " out of order";
    }
}

// ---------------------------------------------------------------------------
// Batched RecordBatch Subscribe over real FastDDS.
//
// Covers two integration-only aspects of the batched path:
//
//   - the batcher's internal timer thread interacting with FastDDS' listener
//     thread (rows arrive from a thread the publisher does not own)
//   - lazy codec acquisition from the SharedSchema delivered with each
//     sample, which is the subscriber-only mode for the batched path
// ---------------------------------------------------------------------------
TEST(PubSubArrowFastDdsTest, BatchedRecordBatchDeliveredAcrossDdsBoundary) {
    FastDDSProviderOptions pub_opts;
    pub_opts.domain_id = kTestDomain;
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(pub_opts));
    FastDDSProviderOptions sub_opts;
    sub_opts.domain_id = kTestDomain;
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(sub_opts));

    // Capture state must outlive `sub`: a delivery on the FastDDS listener
    // thread could otherwise touch destroyed locals during teardown.
    std::mutex mu;
    std::condition_variable cv;
    using BatchStatus = SubscriberArrow::BatchStatus;
    struct Delivery {
        int64_t num_rows;
        size_t num_attachments;
        BatchStatus status;
        std::shared_ptr<arrow::RecordBatch> batch;
    };
    std::vector<Delivery> deliveries;

    PublisherArrow pub(pub_provider);
    SubscriberArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "batched"};

    pub.CreateTopic(topic, schema);

    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 3;
    opt.timeout = std::chrono::seconds(10);  // long, so only the count triggers
    auto result = sub.Subscribe(
        topic,
        [&](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments> att,
            BatchStatus status) {
            std::lock_guard<std::mutex> lk(mu);
            deliveries.push_back(
                {batch ? batch->num_rows() : -1, att.size(), status, std::move(batch)});
            cv.notify_all();
        },
        opt);

    std::shared_ptr<arrow::Schema> sub_schema = result.schema.get();
    ASSERT_NE(sub_schema, nullptr);
    EXPECT_TRUE(sub_schema->Equals(*schema, /*check_metadata=*/false));

    // Publish exactly max_rows samples — RELIABLE + TRANSIENT_LOCAL + KEEP_ALL
    // guarantees ordered delivery to the matched DataReader. Once the batcher
    // has counted three rows on the listener thread, it flushes a kRowLimit
    // batch.
    pub.Publish(topic, SensorRow(0, 0.5, "a"));
    pub.Publish(topic, SensorRow(1, 1.5, "b"));
    pub.Publish(topic, SensorRow(2, 2.5, "c"));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return !deliveries.empty(); }))
            << "batched callback never fired within 10 s";
    }

    // Tear down before reading deliveries so the FastDDS listener thread can
    // no longer mutate the vector.
    sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(deliveries.size(), 1u) << "expected exactly one row-limit flush";
    const Delivery& d = deliveries[0];
    EXPECT_EQ(d.status.reason, BatchStatus::Reason::kRowLimit);
    EXPECT_EQ(d.status.rows_dropped, 0);
    EXPECT_EQ(d.num_rows, 3);
    EXPECT_EQ(d.num_attachments, 3u);

    ASSERT_NE(d.batch, nullptr);
    EXPECT_TRUE(d.batch->schema()->Equals(*schema, /*check_metadata=*/false));

    auto ids = std::static_pointer_cast<arrow::Int32Array>(d.batch->column(0));
    auto temps = std::static_pointer_cast<arrow::DoubleArray>(d.batch->column(1));
    auto labels = std::static_pointer_cast<arrow::StringArray>(d.batch->column(2));
    ASSERT_EQ(ids->length(), 3);
    ASSERT_EQ(temps->length(), 3);
    ASSERT_EQ(labels->length(), 3);

    EXPECT_EQ(ids->Value(0), 0);
    EXPECT_EQ(ids->Value(1), 1);
    EXPECT_EQ(ids->Value(2), 2);
    EXPECT_DOUBLE_EQ(temps->Value(0), 0.5);
    EXPECT_DOUBLE_EQ(temps->Value(1), 1.5);
    EXPECT_DOUBLE_EQ(temps->Value(2), 2.5);
    EXPECT_EQ(labels->GetString(0), "a");
    EXPECT_EQ(labels->GetString(1), "b");
    EXPECT_EQ(labels->GetString(2), "c");
}

// ---------------------------------------------------------------------------
// Subscriber-first over real FastDDS via the Arrow adapter.
//
// Subscribe BEFORE any publisher/CreateTopic exists. SubscriberArrow::Subscribe
// must be non-blocking (the schema future is deferred and not yet ready), and
// once the publisher comes up the row is delivered and the future resolves to
// the publisher's schema. Guards the non-blocking SubscriberArrow contract
// directly over FastDDS (no XRCE Agent) — the companion to the XRCE interop
// test's XrceSubscribeBeforeFastDDSPublish.
// ---------------------------------------------------------------------------
TEST(PubSubArrowFastDdsTest, SubscribeBeforePublishDeliversWithSchema) {
    FastDDSProviderOptions pub_opts;
    pub_opts.domain_id = kTestDomain;
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(pub_opts));
    FastDDSProviderOptions sub_opts;
    sub_opts.domain_id = kTestDomain;
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(std::move(sub_opts));

    std::mutex mu;
    std::condition_variable cv;
    bool received = false;
    ArrowRow rx_row;

    PublisherArrow pub(pub_provider);
    SubscriberArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "subfirst"};

    // Subscribe with no publisher present — must return immediately.
    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        received = true;
        cv.notify_all();
    });

    // The Arrow schema future is deferred and resolves from the publisher's
    // /__schema; with no publisher yet it must not be ready.
    EXPECT_NE(result.schema.wait_for(0s), std::future_status::ready);

    // Bring the publisher up now. RELIABLE + TRANSIENT_LOCAL + KEEP_ALL retains
    // the schema + row for the already-subscribed, late-matching DataReader.
    pub.CreateTopic(topic, schema);
    pub.Publish(topic, SensorRow(42, 23.5, "alpha"));

    // The deferred future resolves once /__schema arrives — guaranteed non-null.
    std::shared_ptr<arrow::Schema> sub_schema = result.schema.get();
    ASSERT_NE(sub_schema, nullptr);
    EXPECT_TRUE(sub_schema->Equals(*schema, /*check_metadata=*/false));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return received; }))
            << "subscriber-first callback never fired within 10 s";
    }
    sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 42);
    EXPECT_EQ(std::static_pointer_cast<arrow::DoubleScalar>(rx_row[1])->value, 23.5);
    EXPECT_EQ(std::static_pointer_cast<arrow::StringScalar>(rx_row[2])->ToString(), "alpha");
}
