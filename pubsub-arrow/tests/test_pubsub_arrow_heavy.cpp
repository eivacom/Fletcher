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

// A row of `count` null elements: the row bitfield (the one field is present), the element count,
// then an all-null element bitfield, nothing else.
size_t RowSize(uint32_t count) { return 1 + sizeof(uint32_t) + (size_t{count} + 7) / 8; }

void WriteRow(uint8_t* out, uint32_t count) {
    out[0] = 0x00;
    std::memcpy(out + 1, &count, sizeof(count));
    std::memset(out + 1 + sizeof(uint32_t), 0xFF, RowSize(count) - 1 - sizeof(uint32_t));
}

}  // namespace

// Control for the test below (its premise P4): the crafted row is refused for the offset budget,
// not as malformed, even by a decoder holding no row.
TEST(SubscriberArrowBatchHeavyTest, CraftedRowExceedsTheOffsetBudgetEvenAlone) {
    std::vector<uint8_t> row(RowSize(kOverBudgetCount));
    WriteRow(row.data(), kOverBudgetCount);
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
        buf.AppendInPlace(RowSize(kOverBudgetCount), [](uint8_t* out, size_t) {
            WriteRow(out, kOverBudgetCount);
            return RowSize(kOverBudgetCount);
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

// ---------------------------------------------------------------------------
// D10C-1 (ruling 2026-11-15, "D10C ladder: the domain-7 port tests", ledger :595 (2)). A row that
// would take a window past Arrow's 32-bit list limit closes the window first and arrives, whole, in
// the next one (d7 L4, L2). Real size, because the limit is Arrow's fixed 2^31 - 2 elements and
// domain 7 has no test hook: two rows of 2^30 null bool elements each fit alone and not together.
// Elements are bool, not int8: null int8 elements are zero-filled, about 1 GiB per decoded row.
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<arrow::Schema> BoolListSchema() {
    return arrow::schema({arrow::field("v", arrow::list(arrow::boolean()))});
}

constexpr uint32_t kHalfBudgetCount = 1u << 30;  // 2^30 elements: two of them are 2^31 > 2^31 - 2

}  // namespace

// Control for the test below (its premise): each row fits an empty decoder, and after one of them
// the other is refused for the budget (BatchCapacityExceeded), not as malformed
// (std::invalid_argument), with nothing appended.
TEST(SubscriberArrowBatchHeavyTest, TwoHalfBudgetRowsFitAloneButNotTogether) {
    std::vector<uint8_t> row(RowSize(kHalfBudgetCount));
    WriteRow(row.data(), kHalfBudgetCount);
    {
        BatchDecoder alone(BoolListSchema());
        EXPECT_NO_THROW(alone.Append(row.data(), row.size()));
        EXPECT_EQ(alone.num_rows(), 1);
    }
    BatchDecoder together(BoolListSchema());
    ASSERT_NO_THROW(together.Append(row.data(), row.size()));
    EXPECT_THROW(together.Append(row.data(), row.size()), BatchCapacityExceeded);
    EXPECT_EQ(together.num_rows(), 1);
}

TEST(SubscriberArrowBatchHeavyTest, ARowThatMeetsTheBudgetClosesTheWindowAndOpensTheNext) {
    struct Rec {
        BatchStatus::Reason reason;
        int64_t num_rows;  // -1 for a null batch
        int64_t dropped;
        size_t attachment_count;
        std::string first_attachment_key;
        int64_t first_row_elements;  // -1 when there is no row
    };
    std::mutex mu;
    std::condition_variable cv;
    std::vector<Rec> recs;  // facts only: holding both ~256 MiB batches would double the peak

    auto provider = std::make_shared<InProcessPubSubProvider>();
    PublisherArrow pub(provider);
    pub.CreateTopic(kTopic, BoolListSchema());
    SubscriberArrow sub(provider);
    SubscriberArrow::BatchOptions opt;
    opt.max_rows = 1000;  // far away: only the offset budget or the close can end a window
    opt.timeout = SubscriberArrow::BatchOptions::kNoTimeout;
    const uint64_t id =
        sub.Subscribe(
               kTopic,
               [&](std::shared_ptr<arrow::RecordBatch> batch, std::vector<Attachments> att,
                   BatchStatus status) {
                   Rec rec{
                       status.reason,
                       batch ? batch->num_rows() : -1,
                       status.rows_dropped,
                       att.size(),
                       att.empty() || att[0].empty() ? std::string{} : std::string(att[0].KeyAt(0)),
                       -1};
                   if (batch && batch->num_rows() > 0) {
                       rec.first_row_elements =
                           std::static_pointer_cast<arrow::ListArray>(batch->column(0))
                               ->value_length(0);
                   }
                   batch.reset();
                   {
                       std::lock_guard<std::mutex> lk(mu);
                       recs.push_back(std::move(rec));
                   }
                   cv.notify_all();
               },
               opt)
            .subscription_id;

    for (const char* key : {"rowA", "rowB"}) {
        Attachments att;
        att.Set(key, Blob{std::vector<uint8_t>{0x01}});
        pub.PublishDirect(
            kTopic,
            [](WriteBuffer& buf) {
                buf.AppendInPlace(RowSize(kHalfBudgetCount), [](uint8_t* out, size_t) {
                    WriteRow(out, kHalfBudgetCount);
                    return RowSize(kHalfBudgetCount);
                });
            },
            att);
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(20), [&] { return !recs.empty(); }))
            << "row B did not close the window holding row A";
        ASSERT_EQ(recs.size(), 1u) << "nothing else may arrive before the close";
        EXPECT_EQ(recs[0].reason, BatchStatus::Reason::kRowLimit);
        EXPECT_EQ(recs[0].num_rows, 1);
        EXPECT_EQ(recs[0].dropped, 0);
        ASSERT_EQ(recs[0].attachment_count, 1u);
        EXPECT_EQ(recs[0].first_attachment_key, "rowA");
        EXPECT_EQ(recs[0].first_row_elements, int64_t{kHalfBudgetCount});
    }

    sub.Unsubscribe(id);  // delivers the window row B opened, as the closing batch

    std::lock_guard<std::mutex> lk(mu);
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_EQ(recs[1].reason, BatchStatus::Reason::kClosing);
    EXPECT_EQ(recs[1].num_rows, 1);
    EXPECT_EQ(recs[1].dropped, 0);
    ASSERT_EQ(recs[1].attachment_count, 1u);
    EXPECT_EQ(recs[1].first_attachment_key, "rowB");
    EXPECT_EQ(recs[1].first_row_elements, int64_t{kHalfBudgetCount});
}
