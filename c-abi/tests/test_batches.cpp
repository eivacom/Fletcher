// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D9S-4a's forcing tests (plans/D9S-4-batches-cross.md, "Forcing-test mapping"): batched
// subscribe through the shim's export table. Domain 9 L12, L14, L15; register BC-9, BC-10, BC-13.
//
// This file is compiled into THREE test executables, one per shim artefact, exactly as
// `test_binding_abi.cpp` is: `c_abi_tests` against the shipped shim (edge in the edge package,
// server in the server package) and `c_abi_probe_tests` against the test-only probe shim (always
// edge). The expectation of each is the literal `FLETCHER_EXPECTED_VARIANT` that
// `tests/CMakeLists.txt` writes by its own mapping and never reads back from the shim: a test that
// took the answer from the shim would agree with the shim whatever the shim said. Rows that are
// about the build get ONE expected answer per artefact and never "either".
//
// The three rows that need a batch to cross exist only in the server binary (`#if` below): an edge
// build has no batcher, so there is nothing for them to assert there, and the fourth row is the one
// that says so, in all three binaries. Columns are int64 and utf8 only (d9 Q11 is open: a batched
// dictionary column arrives re-folded, so none is used). Every call is an `fl_*` entry point; the
// test binary imports with its own Arrow.
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fletcher/abi/binding.h"

#ifndef FLETCHER_EXPECTED_VARIANT
#error "FLETCHER_EXPECTED_VARIANT is not defined: the executable must say which build its shim is (1 = edge, 2 = server). Do not make it optional, and do not accept either."
#endif
static_assert(FLETCHER_EXPECTED_VARIANT == 1 || FLETCHER_EXPECTED_VARIANT == 2,
              "FLETCHER_EXPECTED_VARIANT is one build's wire value: 1 (edge) or 2 (server)");

// The reason values are part of the ABI (L14, review B2): domain 7's three reasons, numbered from
// 1, so that a zeroed field is never a reason. Stated as literals so a renumbering is a red row.
static_assert(FL_BATCH_ROW_LIMIT == 1, "max_rows reached, or the offset budget");
static_assert(FL_BATCH_TIMEOUT == 2, "the window's deadline passed");
static_assert(FL_BATCH_CLOSING == 3, "the subscription is closing");

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr bool kServerBuild = FLETCHER_EXPECTED_VARIANT == 2;

std::string MessageOf(const fl_error& err) {
    if (err.message == nullptr) return {};
    return {reinterpret_cast<const char*>(err.message), err.message_len};
}

fl_str Str(const char* s) { return fl_str{reinterpret_cast<const uint8_t*>(s), std::strlen(s)}; }

/// One thing a batch callback was handed, COPIED out of the borrowed pointers (L14: everything
/// `fl_batch` points to is borrowed for the call). The batch itself is kept the one way the header
/// allows: by Arrow's move. The callback copies each C struct, sets the source's `release` to NULL
/// and imports the copies, so the shim finds nothing left to release when the callback returns.
struct Seen {
    int32_t reason = 0;
    int64_t rows_dropped = -1;
    int64_t num_rows = -1;
    bool had_array = false;
    bool had_schema = false;
    bool array_live_on_entry = false;   // release != NULL when handed over
    bool schema_live_on_entry = false;  // release != NULL when handed over
    bool imported = false;
    std::string import_error;
    std::shared_ptr<arrow::RecordBatch> batch;
    std::thread::id thread;
};

/// The caller's side of a batched subscription: collects what arrives, from whatever thread the
/// shim calls on, and lets the test thread wait ONE-SIDEDLY for a count (the wait ends when the
/// count is reached or the limit passes; no assertion depends on how long it took).
///
/// The callback never throws and never asserts: a gtest ASSERT in a callback returns from a void
/// function and a throw would cross into domain 7. Everything is recorded and judged on the test
/// thread.
class BatchSink {
   public:
    static void OnBatch(void* ctx, const fl_batch* batch) {
        static_cast<BatchSink*>(ctx)->Record(batch);
    }

    bool WaitForDeliveries(size_t count, milliseconds limit) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, limit, [&] { return seen_.size() >= count; });
    }

    bool WaitForRowsDropped(int64_t total, milliseconds limit) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, limit, [&] { return DroppedLocked() >= total; });
    }

    std::vector<Seen> Snapshot() {
        std::lock_guard<std::mutex> lock(mu_);
        return seen_;
    }

    int64_t RowsDropped() {
        std::lock_guard<std::mutex> lock(mu_);
        return DroppedLocked();
    }

   private:
    int64_t DroppedLocked() const {
        int64_t total = 0;
        for (const Seen& s : seen_) total += s.rows_dropped;
        return total;
    }

    void Record(const fl_batch* batch) noexcept {
        Seen s;
        try {
            s.thread = std::this_thread::get_id();
            s.reason = batch->reason;
            s.rows_dropped = batch->rows_dropped;
            s.num_rows = batch->num_rows;
            s.had_array = batch->array != nullptr;
            s.had_schema = batch->schema != nullptr;
            if (s.had_array && s.had_schema) {
                s.array_live_on_entry = batch->array->release != nullptr;
                s.schema_live_on_entry = batch->schema->release != nullptr;
                ArrowArray array = *batch->array;     // the move: copy the struct ...
                ArrowSchema schema = *batch->schema;
                batch->array->release = nullptr;      // ... and mark the source moved-from
                batch->schema->release = nullptr;
                auto imported = arrow::ImportRecordBatch(&array, &schema);
                if (imported.ok()) {
                    s.batch = *imported;
                    s.imported = true;
                } else {
                    s.import_error = imported.status().ToString();
                }
            }
        } catch (...) {
            s.import_error = "the sink threw while recording";
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            seen_.push_back(std::move(s));
        }
        cv_.notify_all();
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Seen> seen_;
};

/// A batch of `seq` (int64) and `label` (utf8) rows, `first_seq` upward, and the whole ABI route
/// to publish it: export, codec and bound rows, released in the order the ABI needs (unbind, close,
/// then the caller's own export).
class PublishedRows {
   public:
    explicit PublishedRows(int64_t first_seq, int64_t count) {
        arrow::Int64Builder seq;
        arrow::StringBuilder label;
        for (int64_t i = 0; i < count; ++i) {
            EXPECT_TRUE(seq.Append(first_seq + i).ok());
            EXPECT_TRUE(label.Append("row-" + std::to_string(first_seq + i)).ok());
        }
        std::shared_ptr<arrow::Array> seq_array;
        std::shared_ptr<arrow::Array> label_array;
        EXPECT_TRUE(seq.Finish(&seq_array).ok());
        EXPECT_TRUE(label.Finish(&label_array).ok());
        batch_ = arrow::RecordBatch::Make(
            arrow::schema({arrow::field("seq", arrow::int64()), arrow::field("label", arrow::utf8())}),
            count, {seq_array, label_array});

        EXPECT_TRUE(arrow::ExportRecordBatch(*batch_, &array_, &schema_).ok());
        fl_error err = {};
        EXPECT_EQ(fl_codec_open(&schema_, &codec_, &err), FL_OK) << MessageOf(err);
        EXPECT_EQ(fl_rows_bind(codec_, &array_, &rows_, &err), FL_OK) << MessageOf(err);
        fl_error_dispose(&err);
    }

    ~PublishedRows() {
        fl_rows_unbind(rows_);
        fl_codec_close(codec_);
        if (array_.release != nullptr) array_.release(&array_);
        if (schema_.release != nullptr) schema_.release(&schema_);
    }

    PublishedRows(const PublishedRows&) = delete;
    PublishedRows& operator=(const PublishedRows&) = delete;

    const arrow::RecordBatch& batch() const { return *batch_; }
    const std::shared_ptr<arrow::RecordBatch>& batch_ptr() const { return batch_; }
    const fl_rows* rows() const { return rows_; }

    /// A fresh export of the schema, for declaring a topic. The caller releases it.
    ArrowSchema ExportSchema() const {
        ArrowSchema out = {};
        EXPECT_TRUE(arrow::ExportSchema(*batch_->schema(), &out).ok());
        return out;
    }

   private:
    std::shared_ptr<arrow::RecordBatch> batch_;
    ArrowSchema schema_ = {};
    ArrowArray array_ = {};
    fl_codec* codec_ = nullptr;
    fl_rows* rows_ = nullptr;
};

/// Provider, publisher and subscriber over `inprocess`, torn down in the order the seam accepts
/// (subscriber, publisher, provider). The subscriber's destruction closes every batched
/// subscription through domain 7 (L9), so a `BatchSink` passed as a callback context must be
/// declared BEFORE the fixture and so outlive it.
class BatchFixture {
   public:
    BatchFixture() {
        const fl_provider_config config = {};
        EXPECT_EQ(fl_provider_create(Str("inprocess"), &config, &provider_, &err_), FL_OK)
            << MessageOf(err_);
        EXPECT_EQ(fl_publisher_create(provider_, &publisher_, &err_), FL_OK) << MessageOf(err_);
        EXPECT_EQ(fl_subscriber_create(provider_, &subscriber_, &err_), FL_OK) << MessageOf(err_);
    }

    /// Destroys the subscriber now. Domain 7 closes every batched subscription first (L9): every
    /// finished batch, then the partial one (reason closing), has been delivered when this returns.
    /// The destructor does not repeat it.
    void DestroySubscriber() {
        fl_subscriber_destroy(subscriber_);
        subscriber_ = nullptr;
    }

    ~BatchFixture() {
        fl_subscriber_destroy(subscriber_);
        fl_publisher_destroy(publisher_);
        fl_provider_destroy(provider_);
        fl_error_dispose(&err_);
    }

    BatchFixture(const BatchFixture&) = delete;
    BatchFixture& operator=(const BatchFixture&) = delete;

    fl_publisher* publisher() const { return publisher_; }
    fl_subscriber* subscriber() const { return subscriber_; }

    void DeclareTopic(fl_topic topic, const PublishedRows& rows) {
        ArrowSchema schema = rows.ExportSchema();
        fl_error err = {};
        EXPECT_EQ(fl_publisher_create_topic(publisher_, topic, &schema, &err), FL_OK)
            << MessageOf(err);
        fl_error_dispose(&err);
        if (schema.release != nullptr) schema.release(&schema);
    }

   private:
    fl_error err_ = {};
    fl_provider* provider_ = nullptr;
    fl_publisher* publisher_ = nullptr;
    fl_subscriber* subscriber_ = nullptr;
};

#if FLETCHER_EXPECTED_VARIANT == 2

// ---------------------------------------------------------------------------
// The server build only (4a)
// ---------------------------------------------------------------------------

/// d9 L14, L15; d7 L4, L5, L9, L12. Five rows published in ONE call, windows of two rows and NO
/// timeout (`INT64_MAX`, d7 L5). With no deadline a window closes only when it is full or when the
/// subscription closes, so the split is the same whatever the scheduler does: the first two windows
/// close by the row limit, the fifth row waits, and closing the subscription (destroying its
/// subscriber, d7 L9) delivers it as the closing batch. Nothing here is a window or a clock of the
/// shim's: the shim hands over what domain 7 closes.
///
/// The one wait is one-sided and asserts no time, and no expected delivery depends on how long
/// anything took. A timeout is deliberately NOT used: a deadline runs from the subscribe, so a
/// stall of that length between subscribing and publishing would split the windows differently.
TEST(Batches, EachClosedWindowCrossesAsAnImportableBatchInWindowOrder) {
    BatchSink sink;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/100, /*count=*/5);

    const fl_str segments[] = {Str("d9s4"), Str("windows")};
    const fl_topic topic = {segments, 2};
    fx.DeclareTopic(topic, published);

    fl_error err = {};
    uint64_t id = 0;
    fl_schema_arrival* arrival = nullptr;
    ASSERT_EQ(fl_subscriber_subscribe_batches(fx.subscriber(), topic, /*max_rows=*/2,
                                              /*timeout_ms=*/INT64_MAX, /*options=*/nullptr,
                                              &BatchSink::OnBatch, &sink, &id, &arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);
    ASSERT_NE(arrival, nullptr) << "out_arrival is written on success";
    EXPECT_NE(id & (uint64_t{1} << 63), 0U)
        << "a batched subscription id has bit 63 set, so it can never equal a row id (review B1)";

    ASSERT_EQ(fl_publisher_publish_rows(fx.publisher(), topic, published.rows(), 0, 5, nullptr, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);

    // The two full windows arrive while the subscription is live.
    ASSERT_TRUE(sink.WaitForDeliveries(2, seconds(10)))
        << "only " << sink.Snapshot().size() << " of 2 full windows arrived within 10 s";
    // Closing delivers the rest, in order, before the destroy returns (d7 L9).
    fx.DestroySubscriber();
    const std::vector<Seen> seen = sink.Snapshot();
    ASSERT_EQ(seen.size(), 3U) << "five rows in windows of two, then the closing batch";

    const int32_t expected_reason[] = {FL_BATCH_ROW_LIMIT, FL_BATCH_ROW_LIMIT, FL_BATCH_CLOSING};
    const int64_t expected_first[] = {0, 2, 4};
    const int64_t expected_rows[] = {2, 2, 1};
    int64_t previous_first_seq = -1;
    for (size_t i = 0; i < seen.size(); ++i) {
        SCOPED_TRACE("delivery " + std::to_string(i));
        const Seen& s = seen[i];
        EXPECT_EQ(s.reason, expected_reason[i]);
        EXPECT_EQ(s.rows_dropped, 0);
        EXPECT_EQ(s.num_rows, expected_rows[i]);
        ASSERT_TRUE(s.had_array && s.had_schema) << "a built window carries an array and a schema";
        EXPECT_TRUE(s.array_live_on_entry) << "the array arrived already released";
        EXPECT_TRUE(s.schema_live_on_entry) << "the schema arrived already released";
        ASSERT_TRUE(s.imported) << "arrow::ImportRecordBatch refused it: " << s.import_error;
        ASSERT_EQ(s.batch->num_rows(), expected_rows[i]);

        // Cell for cell what was published, rows [first, first + n).
        const auto want = published.batch().Slice(expected_first[i], expected_rows[i]);
        EXPECT_TRUE(s.batch->Equals(*want)) << "got:\n"
                                            << s.batch->ToString() << "\nwant:\n"
                                            << want->ToString();

        // Window order: the first seq of each batch strictly increases.
        const auto seq = std::static_pointer_cast<arrow::Int64Array>(s.batch->column(0));
        EXPECT_GT(seq->Value(0), previous_first_seq);
        previous_first_seq = seq->Value(0);
    }

    // One thread for every callback (d7 L12), and not the test's own: the batch thread is
    // domain 7's, so neither the publish nor the closing destroy ran a callback inline.
    for (const Seen& s : seen) {
        EXPECT_EQ(s.thread, seen.front().thread) << "callbacks ran on more than one thread";
        EXPECT_NE(s.thread, std::this_thread::get_id())
            << "a batch was delivered on the thread that published";
    }

    fl_schema_arrival_dispose(arrival);
}

/// d9 L14 and d7 L6, L7: a window domain 7 could not build is not a zero-row batch, it is no batch
/// at all, and its rows are counted (a delivery with nothing pending is never made, d7 L6, so each
/// one here exists because of drops). The topic is never declared, so `inprocess` (as-declared mode)
/// delivers raw rows with no schema and domain 7 has nothing to decode them with: every window is
/// a null batch with its rows in `rows_dropped`.
///
/// The wait is one-sided: the windows may split at any deadline, so only the total is asserted.
TEST(Batches, AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted) {
    BatchSink sink;
    BatchFixture fx;

    const fl_str segments[] = {Str("d9s4"), Str("undeclared")};
    const fl_topic topic = {segments, 2};

    fl_error err = {};
    uint64_t id = 0;
    fl_schema_arrival* arrival = nullptr;
    ASSERT_EQ(fl_subscriber_subscribe_batches(fx.subscriber(), topic, /*max_rows=*/100,
                                              /*timeout_ms=*/200, /*options=*/nullptr,
                                              &BatchSink::OnBatch, &sink, &id, &arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);

    struct Writer {
        static size_t Row(void*, uint8_t* dst, size_t room) {
            if (room < 4) return 0;
            std::memset(dst, 0, 4);
            return 4;
        }
    };
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(fl_publisher_publish_raw(fx.publisher(), topic, &Writer::Row, nullptr, 4, nullptr,
                                           &err),
                  FL_OK)
            << MessageOf(err);
        fl_error_dispose(&err);
    }

    ASSERT_TRUE(sink.WaitForRowsDropped(3, seconds(10)))
        << "only " << sink.RowsDropped() << " of 3 lost rows were reported within 10 s";
    const std::vector<Seen> seen = sink.Snapshot();
    ASSERT_FALSE(seen.empty());
    int64_t total = 0;
    for (size_t i = 0; i < seen.size(); ++i) {
        SCOPED_TRACE("delivery " + std::to_string(i));
        const Seen& s = seen[i];
        EXPECT_FALSE(s.had_array) << "no batch could be built: array is NULL";
        EXPECT_FALSE(s.had_schema) << "schema is NULL exactly when array is";
        EXPECT_EQ(s.num_rows, 0) << "no array, so no rows in it";
        EXPECT_EQ(s.reason, FL_BATCH_TIMEOUT)
            << "three rows against max_rows 100 and no decoder: the deadline is the only event "
               "left that can close the window (d7 L4)";
        EXPECT_GT(s.rows_dropped, 0) << "a no-batch delivery exists to report a loss";
        total += s.rows_dropped;
    }
    EXPECT_EQ(total, 3) << "every published row is counted, none twice";

    fl_schema_arrival_dispose(arrival);
}

#endif  // FLETCHER_EXPECTED_VARIANT == 2

// ---------------------------------------------------------------------------
// All three artefacts: BC-10's truth table for the batched subscribe
// ---------------------------------------------------------------------------

/// d9 L12, L16, BC-10. ONE body, ONE expected answer per artefact (FLETCHER_EXPECTED_VARIANT, from
/// the test build's own mapping): subscribe for batches with a 10 ms timeout, publish one row,
/// then observe for two seconds.
///
///   * edge and probe: FL_NOT_SUPPORTED with FL_ORIGIN_SEAM and a message that names the edge
///     build and `fletcher-c-abi-server`; the caller's outputs are untouched; nothing arrives in
///     the two seconds. A NULL subscriber is the same refusal: the door reads the build before it
///     reads any argument.
///   * server (the control): FL_OK, and one TIMEOUT delivery inside the same two seconds. It shows
///     that the window can see a delivery, so an arm that saw nothing saw nothing for a reason, and
///     that a refusal reading anything but the build turns an arm red. A NULL subscriber is the
///     server's own argument refusal, FL_INVALID_ARGUMENT, which is not the edge's answer.
TEST(Batches, TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery) {
    BatchSink sink;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/7, /*count=*/1);

    const fl_str segments[] = {Str("d9s4"), Str("truth-table")};
    const fl_topic topic = {segments, 2};
    fx.DeclareTopic(topic, published);

    // Sentinels the refusal must leave alone: not zero, not a plausible id, not a real handle.
    constexpr uint64_t kSentinelId = 0x5A5A5A5A5A5A5A5AULL;
    static char sentinel_byte;
    fl_schema_arrival* const kSentinelArrival = reinterpret_cast<fl_schema_arrival*>(&sentinel_byte);
    uint64_t id = kSentinelId;
    fl_schema_arrival* arrival = kSentinelArrival;

    fl_error err = {};
    const fl_status status = fl_subscriber_subscribe_batches(
        fx.subscriber(), topic, /*max_rows=*/100, /*timeout_ms=*/10, /*options=*/nullptr,
        &BatchSink::OnBatch, &sink, &id, &arrival, &err);

    // A NULL subscriber, every other argument valid.
    uint64_t null_id = kSentinelId;
    fl_schema_arrival* null_arrival = kSentinelArrival;
    fl_error null_err = {};
    const fl_status null_status = fl_subscriber_subscribe_batches(
        nullptr, topic, /*max_rows=*/100, /*timeout_ms=*/10, /*options=*/nullptr,
        &BatchSink::OnBatch, &sink, &null_id, &null_arrival, &null_err);

    // One row, then two seconds of looking. On the edge there is no subscription for it to reach;
    // on the server the 10 ms window closes by its deadline.
    fl_error publish_err = {};
    EXPECT_EQ(fl_publisher_publish_row(fx.publisher(), topic, published.rows(), 0, nullptr,
                                       &publish_err),
              FL_OK)
        << MessageOf(publish_err);
    fl_error_dispose(&publish_err);
    const bool delivered = sink.WaitForDeliveries(1, seconds(2));
    const std::vector<Seen> seen = sink.Snapshot();

    if (kServerBuild) {
        EXPECT_EQ(status, FL_OK) << MessageOf(err);
        EXPECT_NE(arrival, kSentinelArrival) << "a served subscribe writes out_arrival";
        EXPECT_NE(id, kSentinelId) << "a served subscribe writes out_id";
        EXPECT_TRUE(delivered) << "the server's window closed by its deadline and nothing arrived";
        ASSERT_EQ(seen.size(), 1U);
        EXPECT_EQ(seen[0].reason, FL_BATCH_TIMEOUT);
        EXPECT_EQ(seen[0].num_rows, 1);
        EXPECT_EQ(seen[0].rows_dropped, 0);

        EXPECT_EQ(null_status, FL_INVALID_ARGUMENT)
            << "the server refuses a NULL subscriber as an argument: " << MessageOf(null_err);
        EXPECT_EQ(null_err.origin, FL_ORIGIN_SEAM);
        EXPECT_EQ(null_id, kSentinelId) << "a refused call writes no output";
        EXPECT_EQ(null_arrival, kSentinelArrival) << "a refused call writes no output";

        if (status == FL_OK && arrival != kSentinelArrival) fl_schema_arrival_dispose(arrival);
    } else {
        EXPECT_EQ(status, FL_NOT_SUPPORTED) << MessageOf(err);
        EXPECT_EQ(err.origin, FL_ORIGIN_SEAM)
            << "the shim's own refusal of a call this build cannot serve is the seam origin";
        const std::string message = MessageOf(err);
        EXPECT_NE(message.find("edge"), std::string::npos) << "message: " << message;
        EXPECT_NE(message.find("fletcher-c-abi-server"), std::string::npos) << "message: " << message;
        EXPECT_EQ(id, kSentinelId) << "a refused call writes no output";
        EXPECT_EQ(arrival, kSentinelArrival) << "a refused call writes no output";
        EXPECT_FALSE(delivered) << "the edge refused, yet a batch arrived";
        EXPECT_TRUE(seen.empty()) << "the edge refused, yet " << seen.size() << " arrived";

        EXPECT_EQ(null_status, FL_NOT_SUPPORTED)
            << "the edge reads the build before any argument: " << MessageOf(null_err);
        EXPECT_EQ(null_err.origin, FL_ORIGIN_SEAM);
        EXPECT_EQ(null_id, kSentinelId) << "a refused call writes no output";
        EXPECT_EQ(null_arrival, kSentinelArrival) << "a refused call writes no output";
    }

    fl_error_dispose(&err);
    fl_error_dispose(&null_err);
}

}  // namespace
