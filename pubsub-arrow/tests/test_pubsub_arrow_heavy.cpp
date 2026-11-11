// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Tests that allocate hundreds of MiB. Built as their own executable and labelled `heavy`, so the
// default run (`ctest -LE heavy`) leaves them out (owner ruling 2026-11-10, "Label it 'heavy'";
// domain 7 D11). Run them with `ctest -L heavy`.
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fletcher/arrow_bridge/batch_decoder.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <memory>
#include <mutex>
#include <vector>

using namespace fletcher;

namespace {

using BatchStatus = SubscriberArrow::BatchStatus;

const std::vector<std::string> kTopic = {"heavy", "topic"};

std::shared_ptr<arrow::Schema> ListSchema() {
    return arrow::schema({arrow::field("v", arrow::list(arrow::int8()))});
}

// A well-formed list<int8> row whose element count, 2^31 - 1, is over Arrow's 32-bit list builder
// limit (2^31 - 2 elements) on its own, so it meets the offset budget even in an empty window. The
// elements are all null, so the row is the element bitfield and nothing else: ~256 MiB.
constexpr uint32_t kOverBudgetCount = 0x7FFFFFFFu;

size_t OverBudgetRowSize() { return 1 + sizeof(uint32_t) + (size_t{kOverBudgetCount} + 7) / 8; }

void WriteOverBudgetRow(uint8_t* out) {
    out[0] = 0x00;  // row bitfield: the one field is present
    std::memcpy(out + 1, &kOverBudgetCount, sizeof(kOverBudgetCount));
    std::memset(out + 1 + sizeof(uint32_t), 0xFF, OverBudgetRowSize() - 1 - sizeof(uint32_t));
}

}  // namespace

// Control for the test below (its premise P4): the crafted row is refused for the offset budget,
// not as malformed, even by a decoder holding no row.
TEST(SubscriberArrowBatchHeavyTest, CraftedRowExceedsTheOffsetBudgetEvenAlone) {
    std::vector<uint8_t> row(OverBudgetRowSize());
    WriteOverBudgetRow(row.data());
    BatchDecoder decoder(ListSchema());
    EXPECT_THROW(decoder.Append(row.data(), row.size()), BatchCapacityExceeded);
    EXPECT_EQ(decoder.num_rows(), 0);
}

// Rung 5 (L1 violation 2, L2, L4). One good row, then the over-budget row. The window cannot take
// it, so it closes early: the first delivery is kRowLimit holding only the good row, although
// max_rows is far away. That callback unsubscribes. The over-budget row, which does not fit even
// alone, must still be reported: the kClosing batch carries it in rows_dropped. Rows received plus
// rows reported lost equal rows sent (2).
TEST(SubscriberArrowBatchHeavyTest, OverBudgetRowAroundInCallbackUnsubscribeIsCounted) {
    struct Rec {
        BatchStatus::Reason reason;
        int64_t num_rows;  // -1 for a null batch
        int64_t dropped;
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Rec> recs;
    std::unique_ptr<SubscriberArrow> sub;
    uint64_t id = 0;

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, ListSchema());
    sub = std::make_unique<SubscriberArrow>(provider);
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 1000;  // far away: only the offset budget (or the close) can close a window
    opt.timeout = std::chrono::minutes(10);
    id = sub->Subscribe(
                kTopic,
                [&](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments>,
                    BatchStatus status) {
                    bool first;
                    {
                        std::lock_guard<std::mutex> lk(mu);
                        first = recs.empty();
                        recs.push_back(
                            {status.reason, batch ? batch->num_rows() : -1, status.rows_dropped});
                    }
                    cv.notify_all();
                    if (first) sub->Unsubscribe(id);
                },
                opt)
             .subscription_id;

    arrow::Int8Builder values;
    ASSERT_TRUE(values.AppendValues({1, 2}).ok());
    pub.Publish(kTopic, {std::make_shared<arrow::ListScalar>(values.Finish().ValueOrDie())});
    pub.PublishDirect(kTopic, [](WriteBuffer& buf) {
        buf.AppendInPlace(OverBudgetRowSize(), [](uint8_t* out, size_t) {
            WriteOverBudgetRow(out);
            return OverBudgetRowSize();
        });
    });

    {
        std::unique_lock<std::mutex> lk(mu);
        EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(5), [&] { return recs.size() >= 2; }))
            << "no closing batch: the over-budget row was not reported";
    }
    sub.reset();

    std::lock_guard<std::mutex> lk(mu);
    ASSERT_GE(recs.size(), 1u);
    EXPECT_EQ(recs[0].reason, BatchStatus::Reason::kRowLimit);
    EXPECT_EQ(recs[0].num_rows, 1) << "the window did not close early on the offset budget";
    EXPECT_EQ(recs[0].dropped, 0);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[1].reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(recs[1].num_rows, 0);
    EXPECT_EQ(recs[1].dropped, 1);
    int64_t accounted = 0;
    for (const auto& r : recs) accounted += (r.num_rows > 0 ? r.num_rows : 0) + r.dropped;
    EXPECT_EQ(accounted, 2) << "rows received + rows reported lost != rows sent";
}
