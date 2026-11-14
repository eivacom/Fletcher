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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fletcher/abi/binding.h"

#if FLETCHER_EXPECTED_VARIANT == 2
// Rung 7's oracle only (D9S-5a): domain 7, linked by the server test binary alone.
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#endif

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
    // D9S-4b: each row's attachments, COPIED during the callback (keys and bytes), one entry per
    // row, in row order. A NULL set is counted, not hidden: it is a forbidden state (d9 L14).
    using Entries = std::vector<std::pair<std::string, std::string>>;
    bool had_attachments = false;  // fl_batch::attachments != NULL
    size_t null_attachment_sets = 0;
    std::vector<Entries> attachments;
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
            s.had_attachments = batch->attachments != nullptr;
            if (s.had_attachments) {
                for (int64_t row = 0; row < batch->num_rows; ++row) {
                    const fl_attachments* set = batch->attachments[row];
                    Seen::Entries entries;
                    if (set == nullptr) {
                        ++s.null_attachment_sets;
                    } else {
                        for (size_t i = 0; i < fl_attachments_size(set); ++i) {
                            const fl_str key = fl_attachments_key_at(set, i);
                            const fl_blob value = fl_attachments_value_at(set, i);
                            entries.emplace_back(
                                std::string(reinterpret_cast<const char*>(key.data), key.len),
                                value.size == 0
                                    ? std::string()
                                    : std::string(reinterpret_cast<const char*>(value.data),
                                                  value.size));
                        }
                    }
                    s.attachments.push_back(std::move(entries));
                }
            }
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

    fl_provider* provider() const { return provider_; }
    fl_publisher* publisher() const { return publisher_; }
    fl_subscriber* subscriber() const { return subscriber_; }

    /// Destroys every handle now, the provider first or last (d9 L3: release order between handles
    /// never matters to the shim). The destructor does not repeat it.
    void DestroyAll(bool provider_first) {
        if (provider_first) fl_provider_destroy(std::exchange(provider_, nullptr));
        fl_subscriber_destroy(std::exchange(subscriber_, nullptr));
        fl_publisher_destroy(std::exchange(publisher_, nullptr));
        fl_provider_destroy(std::exchange(provider_, nullptr));
    }

    void Publish(fl_topic topic, const PublishedRows& rows, int64_t first, int64_t count) {
        fl_error err = {};
        EXPECT_EQ(fl_publisher_publish_rows(publisher_, topic, rows.rows(), first, count, nullptr, &err),
                  FL_OK)
            << MessageOf(err);
        fl_error_dispose(&err);
    }

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

// ---------------------------------------------------------------------------
// D9S-5a helpers (plans/D9S-5a-close-release-options.md, "Forcing-test mapping")
// ---------------------------------------------------------------------------

constexpr uint64_t kBatchedIdTag = uint64_t{1} << 63;

/// A two-segment topic that owns its strings, so a test can make many. Neither copied nor moved:
/// the `fl_str`s point into the strings.
class OwnedTopic {
   public:
    OwnedTopic(std::string a, std::string b) : parts_{std::move(a), std::move(b)} {
        for (const std::string& p : parts_) segs_.push_back(Str(p.c_str()));
    }
    OwnedTopic(const OwnedTopic&) = delete;
    OwnedTopic& operator=(const OwnedTopic&) = delete;
    fl_topic topic() const { return {segs_.data(), segs_.size()}; }

   private:
    std::vector<std::string> parts_;
    std::vector<fl_str> segs_;
};

/// A per-row subscription's caller: counts deliveries; the wait is one-sided.
class RowSink {
   public:
    static void OnRow(void* ctx, uint64_t, const uint8_t*, size_t, const fl_schema*,
                      const fl_attachments*) {
        auto* self = static_cast<RowSink*>(ctx);
        {
            std::lock_guard<std::mutex> lock(self->mu_);
            ++self->count_;
        }
        self->cv_.notify_all();
    }
    bool WaitFor(size_t count, milliseconds limit) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, limit, [&] { return count_ >= count; });
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    size_t count_ = 0;
};

/// Subscribes `sink` for batches, disposes the arrival (the caller's, and not needed here) and
/// returns the id.
uint64_t SubscribeBatches(fl_subscriber* subscriber, fl_topic topic, int64_t max_rows,
                          int64_t timeout_ms, BatchSink& sink) {
    fl_error err = {};
    uint64_t id = 0;
    fl_schema_arrival* arrival = nullptr;
    EXPECT_EQ(fl_subscriber_subscribe_batches(subscriber, topic, max_rows, timeout_ms, nullptr,
                                              &BatchSink::OnBatch, &sink, &id, &arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);
    if (arrival != nullptr) fl_schema_arrival_dispose(arrival);
    return id;
}

/// The batched close, its status asserted.
void CloseBatches(fl_subscriber* subscriber, uint64_t id, fl_status expected) {
    fl_error err = {};
    EXPECT_EQ(fl_subscriber_unsubscribe_batches(subscriber, id, &err), expected) << MessageOf(err);
    fl_error_dispose(&err);
}

/// One expected delivery: its reason, and the published rows [first, first + rows) it carries.
struct Window {
    int32_t reason;
    int64_t first;
    int64_t rows;
};

/// Exactly these deliveries, in this order: reason, row count, nothing dropped, and cell for cell
/// the published rows.
void ExpectWindows(const std::vector<Seen>& seen, const PublishedRows& published,
                   const std::vector<Window>& want) {
    ASSERT_EQ(seen.size(), want.size()) << "deliveries";
    for (size_t i = 0; i < seen.size(); ++i) {
        SCOPED_TRACE("delivery " + std::to_string(i));
        EXPECT_EQ(seen[i].reason, want[i].reason);
        EXPECT_EQ(seen[i].rows_dropped, 0);
        EXPECT_EQ(seen[i].num_rows, want[i].rows);
        ASSERT_TRUE(seen[i].imported) << seen[i].import_error;
        EXPECT_TRUE(seen[i].batch->Equals(*published.batch().Slice(want[i].first, want[i].rows)))
            << seen[i].batch->ToString();
    }
}

/// The edge build's one answer to a batched entry point (d9 L12, BC-10), shared by both truth
/// tables (design debt DEBT-4).
void ExpectRefusedAsEdge(fl_status status, const fl_error& err) {
    EXPECT_EQ(status, FL_NOT_SUPPORTED) << MessageOf(err);
    EXPECT_EQ(err.origin, FL_ORIGIN_SEAM)
        << "the shim's own refusal of a call this build cannot serve is the seam origin";
    const std::string message = MessageOf(err);
    EXPECT_NE(message.find("edge"), std::string::npos) << "message: " << message;
    EXPECT_NE(message.find("fletcher-c-abi-server"), std::string::npos) << "message: " << message;
}

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
/// at all, and its rows are counted (while open, a deadline with nothing pending delivers nothing,
/// d7 L6, so each delivery here exists because of drops; the snapshot is taken before the close,
/// whose empty closing delivery is not this test's subject). The topic is never declared, so `inprocess` (as-declared mode)
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
        EXPECT_FALSE(s.had_attachments) << "no rows, so the attachments list is NULL (d9 L14)";
        EXPECT_EQ(s.reason, FL_BATCH_TIMEOUT)
            << "three rows against max_rows 100 and no decoder: the deadline is the only event "
               "left that can close the window (d7 L4)";
        EXPECT_GT(s.rows_dropped, 0) << "a no-batch delivery exists to report a loss";
        total += s.rows_dropped;
    }
    EXPECT_EQ(total, 3) << "every published row is counted, none twice";

    fl_schema_arrival_dispose(arrival);
}

/// A set built through the shim's own builder, for the publish side of the test. The builder
/// retains each value, so the test releases its own blob reference at once.
using SetPtr = std::unique_ptr<fl_attachments, void (*)(fl_attachments*)>;

SetPtr MakeSet(const Seen::Entries& entries) {
    fl_error err = {};
    fl_attachments_builder* builder = nullptr;
    fl_attachments* set = nullptr;
    EXPECT_EQ(fl_attachments_builder_create(&builder, &err), FL_OK) << MessageOf(err);
    for (const auto& [key, value] : entries) {
        fl_blob blob = {};
        EXPECT_EQ(fl_blob_create(reinterpret_cast<const uint8_t*>(value.data()), value.size(), &blob,
                                 &err),
                  FL_OK)
            << MessageOf(err);
        EXPECT_EQ(fl_attachments_builder_set(
                      builder, fl_str{reinterpret_cast<const uint8_t*>(key.data()), key.size()}, &blob,
                      &err),
                  FL_OK)
            << MessageOf(err);
        fl_blob_release(&blob);
    }
    EXPECT_EQ(fl_attachments_builder_build(builder, &set, &err), FL_OK) << MessageOf(err);
    fl_attachments_builder_dispose(builder);
    fl_error_dispose(&err);
    return SetPtr(set, &fl_attachments_dispose);
}

/// d9 L14 (attachments clause), d7 L3, L15; register BC-13. Rows 100-104 in windows of two, no
/// timeout, closed by destroying the subscriber, exactly as the test above. Row 1 carries
/// {trace: r1} and row 2 carries {origin: o2, trace: r2}, published through `atts_per_row`; the
/// other three carry none. Row 1 is the LAST row of window 1 and row 2 the FIRST of window 2, so a
/// set landing on the wrong row inside a window, or slipping across the window boundary, is a
/// different answer. Keys, values and set sizes are all distinct (0, 1 and 2 entries; 3, 5 and 2
/// bytes, one with an embedded zero), so an all-empty, all-same, shifted or swapped answer fails.
///
///   window 1 (rows 0,1): [ {},                    {trace: r1} ]
///   window 2 (rows 2,3): [ {origin: o2, trace: r2}, {}        ]
///   closing  (row 4):    [ {} ]
///
/// A row without attachments gets an EMPTY set, never a NULL one; the list is non-NULL whenever
/// the batch has rows. The sink copied every set during its callback, so nothing here reads a
/// borrowed pointer after the call returned.
TEST(Batches, EachBatchRowCarriesItsOwnAttachments) {
    BatchSink sink;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/100, /*count=*/5);

    const fl_str segments[] = {Str("d9s4"), Str("attachments")};
    const fl_topic topic = {segments, 2};
    fx.DeclareTopic(topic, published);

    const std::string r1("\x01\x00\x02", 3);
    const std::string o2("o2-by", 5);
    const std::string r2("r2", 2);
    const SetPtr row1 = MakeSet({{"trace", r1}});
    const SetPtr row2 = MakeSet({{"origin", o2}, {"trace", r2}});
    ASSERT_NE(row1, nullptr);
    ASSERT_NE(row2, nullptr);
    const fl_attachments* const per_row[5] = {nullptr, row1.get(), row2.get(), nullptr, nullptr};

    fl_error err = {};
    uint64_t id = 0;
    fl_schema_arrival* arrival = nullptr;
    ASSERT_EQ(fl_subscriber_subscribe_batches(fx.subscriber(), topic, /*max_rows=*/2,
                                              /*timeout_ms=*/INT64_MAX, /*options=*/nullptr,
                                              &BatchSink::OnBatch, &sink, &id, &arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);

    ASSERT_EQ(fl_publisher_publish_rows(fx.publisher(), topic, published.rows(), 0, 5, per_row, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);

    ASSERT_TRUE(sink.WaitForDeliveries(2, seconds(10)))
        << "only " << sink.Snapshot().size() << " of 2 full windows arrived within 10 s";
    fx.DestroySubscriber();
    const std::vector<Seen> seen = sink.Snapshot();
    ASSERT_EQ(seen.size(), 3U) << "five rows in windows of two, then the closing batch";

    const std::vector<std::vector<Seen::Entries>> expected = {
        {{}, {{"trace", r1}}},
        {{{"origin", o2}, {"trace", r2}}, {}},
        {{}},
    };
    for (size_t i = 0; i < seen.size(); ++i) {
        SCOPED_TRACE("delivery " + std::to_string(i));
        const Seen& s = seen[i];
        EXPECT_EQ(s.rows_dropped, 0);
        EXPECT_EQ(s.num_rows, static_cast<int64_t>(expected[i].size()));
        EXPECT_TRUE(s.had_attachments) << "a batch with rows carries its attachments list";
        EXPECT_EQ(s.null_attachment_sets, 0U) << "a row with nothing gets an empty set, never NULL";
        EXPECT_EQ(s.attachments, expected[i]);
    }

    fl_schema_arrival_dispose(arrival);
}

// ---------------------------------------------------------------------------
// The server build only (5a)
// ---------------------------------------------------------------------------

/// Rung 6 from outside a callback; d9 L15, d7 L9; ruling 2026-11-14 ("exactly one closing
/// delivery"). Run once with each closer. Five rows in windows of two, no timeout, closed at once:
/// WHEN THE CALL RETURNS, with no wait, ROW_LIMIT, ROW_LIMIT, CLOSING (row 5) have arrived, in
/// order. Two more rows then reach no one in 2 s, while a live subscription on another topic gets a
/// ROW_LIMIT window inside the same 2 s (the control). The close's live subscription shares the
/// subscriber, so a close that ended the whole host turns it red; a destroy ends every
/// subscription of its subscriber, so its control is on a second one (DEBT-2). Each subscription
/// has its own sink and every count is asserted before teardown, which closes the live ones
/// unasserted (what an idle close delivers is `EveryCloseEndsWithExactlyOneClosingDelivery`'s).
TEST(Batches, ClosingFromOutsideDeliversTheRestThenTheClosingBatch) {
    for (const bool by_destroy : {false, true}) {
        SCOPED_TRACE(by_destroy ? "fl_subscriber_destroy" : "fl_subscriber_unsubscribe_batches");
        BatchSink sink;
        BatchSink control;
        BatchFixture fx;
        PublishedRows published(/*first_seq=*/100, /*count=*/5);
        const OwnedTopic closed("d9s5", "closed");
        const OwnedTopic live("d9s5", "live");
        fx.DeclareTopic(closed.topic(), published);
        fx.DeclareTopic(live.topic(), published);

        fl_subscriber* second = nullptr;
        if (by_destroy) {
            fl_error err = {};
            ASSERT_EQ(fl_subscriber_create(fx.provider(), &second, &err), FL_OK) << MessageOf(err);
        }
        const std::unique_ptr<fl_subscriber, void (*)(fl_subscriber*)> second_guard(
            second, &fl_subscriber_destroy);

        const uint64_t id = SubscribeBatches(fx.subscriber(), closed.topic(), 2, INT64_MAX, sink);
        SubscribeBatches(by_destroy ? second : fx.subscriber(), live.topic(), 2, INT64_MAX, control);

        fx.Publish(closed.topic(), published, 0, 5);
        if (by_destroy) {
            fx.DestroySubscriber();
        } else {
            CloseBatches(fx.subscriber(), id, FL_OK);
        }
        ExpectWindows(sink.Snapshot(), published,
                      {{FL_BATCH_ROW_LIMIT, 0, 2}, {FL_BATCH_ROW_LIMIT, 2, 2}, {FL_BATCH_CLOSING, 4, 1}});

        fx.Publish(closed.topic(), published, 0, 2);
        fx.Publish(live.topic(), published, 0, 2);
        EXPECT_FALSE(sink.WaitForDeliveries(4, seconds(2))) << "a delivery came after the close";
        ExpectWindows(control.Snapshot(), published, {{FL_BATCH_ROW_LIMIT, 0, 2}});

        if (!by_destroy) {
            CloseBatches(fx.subscriber(), id, FL_OK);  // not live: a no-op (H1)
            const std::vector<Seen> seen = sink.Snapshot();
            EXPECT_EQ(seen.size(), 3U);
            EXPECT_EQ(std::count_if(seen.begin(), seen.end(),
                                    [](const Seen& s) { return s.reason == FL_BATCH_CLOSING; }),
                      1)
                << "exactly one closing delivery";
        }
    }
}

/// G1's caller (rung 5): per window it MOVES both structs, LEAVES both, or moves the array and
/// leaves the schema. A struct left in place is first moved into the test's own stash, and the
/// in-place copy gets a counting `release` whose state is the test's: a second call is counted,
/// never a use-after-free. It records how many times it ran, whether its window's callback had
/// returned, and on which thread.
class MoveOrLeaveSink {
   public:
    enum class Keep { kBoth, kNeither, kArrayOnly };
    struct Counter {
        std::atomic<int> calls{0};
        std::atomic<int> before_return{0};
        std::thread::id thread;
        const std::atomic<bool>* returned = nullptr;
    };
    struct Taken {
        int32_t reason = 0;
        std::thread::id thread;
        ArrowArray array = {};    // moved, or the stashed original
        ArrowSchema schema = {};  // moved, or the stashed original
        Counter array_release;
        Counter schema_release;
        std::atomic<bool> returned{false};
    };
    static constexpr Keep kPlan[3] = {Keep::kBoth, Keep::kNeither, Keep::kArrayOnly};

    static void OnBatch(void* ctx, const fl_batch* batch) {
        auto* self = static_cast<MoveOrLeaveSink*>(ctx);
        const size_t i = self->count_.fetch_add(1);
        if (i >= 3 || batch->array == nullptr || batch->schema == nullptr) return;
        Taken& t = self->taken_[i];
        t.reason = batch->reason;
        t.thread = std::this_thread::get_id();
        t.array = *batch->array;
        t.schema = *batch->schema;
        if (kPlan[i] == Keep::kNeither) {
            Leave(batch->array, t.array_release, t.returned);
        } else {
            batch->array->release = nullptr;
        }
        if (kPlan[i] == Keep::kBoth) {
            batch->schema->release = nullptr;
        } else {
            Leave(batch->schema, t.schema_release, t.returned);
        }
        t.returned.store(true);  // the callback's last act
    }

    size_t count() const { return count_.load(); }
    Taken& taken(size_t i) { return taken_[i]; }

   private:
    template <class T>
    static void Leave(T* in_place, Counter& counter, const std::atomic<bool>& returned) {
        counter.returned = &returned;
        in_place->private_data = &counter;
        in_place->release = &Count<T>;
    }
    template <class T>
    static void Count(T* s) {
        auto* c = static_cast<Counter*>(s->private_data);
        if (!c->returned->load()) c->before_return.fetch_add(1);
        c->thread = std::this_thread::get_id();
        c->calls.fetch_add(1);
        s->release = nullptr;  // marked released, as the C Data Interface requires
    }

    std::atomic<size_t> count_{0};
    std::array<Taken, 3> taken_;
};

/// Rung 5; d9 L14 (D26), L11, L3; register BC-13 (the batch half, counted not measured: see the
/// design's Risks). A GUARD: green on the pre-stage tree apart from the close. Windows as in the
/// forcing test: W0 moved whole, W1 left whole, W2 array moved and schema left. Close, then
/// destroy the subscriber and the provider, in both orders. Then, on a new thread, import W0 and
/// W2's array, compare them cell for cell, and release them. Each counting `release` ran EXACTLY
/// ONCE, after its callback returned, on the batch thread. Mutation controls (design): delete
/// `ReleaseUnmoved` → 0 calls; release unconditionally → a call through NULL; release twice → 2.
TEST(Batches, ABatchIsTheCallersOnlyIfMoved) {
    for (const bool provider_first : {false, true}) {
        SCOPED_TRACE(provider_first ? "provider destroyed first" : "provider destroyed last");
        MoveOrLeaveSink sink;
        PublishedRows published(/*first_seq=*/100, /*count=*/5);
        auto fx = std::make_unique<BatchFixture>();
        const OwnedTopic topic("d9s5", "move-or-leave");
        fx->DeclareTopic(topic.topic(), published);

        fl_error err = {};
        uint64_t id = 0;
        fl_schema_arrival* arrival = nullptr;
        ASSERT_EQ(fl_subscriber_subscribe_batches(fx->subscriber(), topic.topic(), 2, INT64_MAX,
                                                  nullptr, &MoveOrLeaveSink::OnBatch, &sink, &id,
                                                  &arrival, &err),
                  FL_OK)
            << MessageOf(err);
        fl_error_dispose(&err);
        fl_schema_arrival_dispose(arrival);
        fx->Publish(topic.topic(), published, 0, 5);
        CloseBatches(fx->subscriber(), id, FL_OK);
        fx->DestroyAll(provider_first);
        fx.reset();
        ASSERT_EQ(sink.count(), 3U) << "deliveries";

        std::thread([&] {
            const int64_t first[] = {0, 2, 4};
            const int64_t count[] = {2, 2, 1};
            for (size_t i : {size_t{0}, size_t{2}}) {
                SCOPED_TRACE("window " + std::to_string(i));
                MoveOrLeaveSink::Taken& t = sink.taken(i);
                auto imported = arrow::ImportRecordBatch(&t.array, &t.schema);
                ASSERT_TRUE(imported.ok()) << imported.status().ToString();
                EXPECT_TRUE((*imported)->Equals(*published.batch().Slice(first[i], count[i])))
                    << (*imported)->ToString();
            }
            MoveOrLeaveSink::Taken& w1 = sink.taken(1);
            w1.array.release(&w1.array);    // the stashed originals are the test's
            w1.schema.release(&w1.schema);
        }).join();

        const std::pair<const char*, MoveOrLeaveSink::Counter*> left[] = {
            {"W1 array", &sink.taken(1).array_release},
            {"W1 schema", &sink.taken(1).schema_release},
            {"W2 schema", &sink.taken(2).schema_release}};
        const std::thread::id batch_thread = sink.taken(0).thread;
        for (const auto& [name, counter] : left) {
            SCOPED_TRACE(name);
            EXPECT_EQ(counter->calls.load(), 1) << "the shim releases what was not moved exactly once";
            EXPECT_EQ(counter->before_return.load(), 0) << "released before the callback returned";
            EXPECT_EQ(counter->thread, batch_thread) << "released off the batch thread";
        }
        EXPECT_NE(batch_thread, std::this_thread::get_id());
        const int32_t reasons[] = {FL_BATCH_ROW_LIMIT, FL_BATCH_ROW_LIMIT, FL_BATCH_CLOSING};
        for (size_t i = 0; i < 3; ++i) EXPECT_EQ(sink.taken(i).reason, reasons[i]) << "window " << i;
    }
}

/// The oracle (rung 7): domain 7's own refusal of these options, through a `SubscriberArrow` over
/// the test's own `inprocess` provider. It computes the expected message only; every exercise in
/// the test below is an `fl_*` call.
std::string Domain7Refusal(int64_t max_rows, int64_t timeout_ms) {
    fletcher::SubscriberArrow d7(std::make_shared<fletcher::InProcessPubSubProvider>());
    try {
        (void)d7.Subscribe({"d9s5", "oracle"},
                           [](std::shared_ptr<arrow::RecordBatch>, std::vector<fletcher::Attachments>,
                              fletcher::SubscriberArrow::BatchStatus) {},
                           fletcher::SubscriberArrow::BatchOptions{
                               max_rows, std::chrono::milliseconds(timeout_ms)});
    } catch (const fletcher::PubSubError& e) {
        return e.what();
    }
    return "<domain 7 accepted these options>";
}

/// Rung 7; d9 L15, L4; d7 L15, L5; register BC-8, BC-14. A GUARD: green on the pre-stage tree
/// apart from the close. d7's 6 x 7 grid, in int64_t, one topic and one sink per cell, one row
/// each, one 2 s window. Refused (27) exactly when max_rows < 1 || timeout < 0: FL_INVALID_ARGUMENT
/// at SEAM, domain 7's message byte for byte, OUT sentinels untouched, nothing delivered. Accepted
/// (15): max_rows 1 → ROW_LIMIT; timeouts 0 and 1 → TIMEOUT; the six cells with no deadline in
/// reach (INT64_MAX is BC-14's "none"; 100 years and the clock's maximum never fire early, d7 L5)
/// deliver nothing in the window, then exactly one CLOSING with the row when the close returns.
/// Timing control: a 10 ms subscription gives TIMEOUT inside the same window. Topic options: a
/// max_payload_bytes the row subscribe refuses gets that same status and message.
TEST(Batches, EveryBatchOptionReachesDomain7AsGiven) {
    const int64_t row_counts[] = {INT64_MIN, -1, 0, 1, 2, INT64_MAX};
    const int64_t timeouts[] = {
        INT64_MIN, -1, 0, 1, INT64_MAX, 3'155'760'000'000LL /* 100 years */,
        std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::duration::max()).count()};
    struct Cell {
        int64_t max_rows;
        int64_t timeout_ms;
        fl_status status;
        int32_t origin;
        std::string message;
        uint64_t id;
        fl_schema_arrival* arrival;
    };
    constexpr uint64_t kSentinelId = 0x5A5A5A5A5A5A5A5AULL;
    static char sentinel_byte;
    fl_schema_arrival* const kSentinelArrival = reinterpret_cast<fl_schema_arrival*>(&sentinel_byte);

    std::deque<BatchSink> sinks;  // one per cell, then the control's; they outlive the fixture
    RowSink rows;
    std::deque<OwnedTopic> topics;
    std::vector<Cell> cells;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/1, /*count=*/1);

    for (const int64_t max_rows : row_counts) {
        for (const int64_t timeout_ms : timeouts) {
            topics.emplace_back("d9s5-grid", std::to_string(cells.size()));
            sinks.emplace_back();
            fx.DeclareTopic(topics.back().topic(), published);
            Cell c{max_rows, timeout_ms, FL_OK, 0, {}, kSentinelId, kSentinelArrival};
            fl_error err = {};
            c.status = fl_subscriber_subscribe_batches(fx.subscriber(), topics.back().topic(), max_rows,
                                                       timeout_ms, nullptr, &BatchSink::OnBatch,
                                                       &sinks.back(), &c.id, &c.arrival, &err);
            c.origin = err.origin;
            c.message = MessageOf(err);
            fl_error_dispose(&err);
            if (c.status == FL_OK) fl_schema_arrival_dispose(c.arrival);
            cells.push_back(std::move(c));
        }
    }
    topics.emplace_back("d9s5-grid", "control");
    sinks.emplace_back();
    BatchSink& control = sinks.back();
    fx.DeclareTopic(topics.back().topic(), published);
    SubscribeBatches(fx.subscriber(), topics.back().topic(), 100, 10, control);

    for (const OwnedTopic& t : topics) fx.Publish(t.topic(), published, 0, 1);
    // The one observation window: wait one-sidedly for each expected delivery, then sleep out
    // the rest of the 2 s before the cells that must see nothing are judged.
    const auto window_end = std::chrono::steady_clock::now() + seconds(2);
    const auto left = [&] {
        return std::chrono::duration_cast<milliseconds>(window_end - std::chrono::steady_clock::now());
    };
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        if (c.max_rows >= 1 && c.timeout_ms >= 0 && (c.max_rows == 1 || c.timeout_ms <= 1)) {
            sinks[i].WaitForDeliveries(1, left());
        }
    }
    control.WaitForDeliveries(1, left());
    std::this_thread::sleep_until(window_end);

    int refused = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        SCOPED_TRACE("max_rows " + std::to_string(c.max_rows) + ", timeout_ms " +
                     std::to_string(c.timeout_ms));
        if (c.max_rows < 1 || c.timeout_ms < 0) {
            ++refused;
            EXPECT_EQ(c.status, FL_INVALID_ARGUMENT) << c.message;
            EXPECT_EQ(c.origin, FL_ORIGIN_SEAM);
            EXPECT_EQ(c.message, Domain7Refusal(c.max_rows, c.timeout_ms)) << "byte for byte";
            EXPECT_EQ(c.id, kSentinelId) << "a refused call writes no output";
            EXPECT_EQ(c.arrival, kSentinelArrival) << "a refused call writes no output";
            EXPECT_TRUE(sinks[i].Snapshot().empty()) << "a refused subscription delivered";
        } else if (c.max_rows == 1) {
            EXPECT_EQ(c.status, FL_OK) << c.message;
            ExpectWindows(sinks[i].Snapshot(), published, {{FL_BATCH_ROW_LIMIT, 0, 1}});
        } else if (c.timeout_ms <= 1) {
            EXPECT_EQ(c.status, FL_OK) << c.message;
            ExpectWindows(sinks[i].Snapshot(), published, {{FL_BATCH_TIMEOUT, 0, 1}});
        } else {
            EXPECT_EQ(c.status, FL_OK) << c.message;
            EXPECT_TRUE(sinks[i].Snapshot().empty()) << "a window closed with no deadline in reach";
            CloseBatches(fx.subscriber(), c.id, FL_OK);
            ExpectWindows(sinks[i].Snapshot(), published, {{FL_BATCH_CLOSING, 0, 1}});
        }
    }
    EXPECT_EQ(cells.size(), 42U);
    EXPECT_EQ(refused, 27);
    ExpectWindows(control.Snapshot(), published, {{FL_BATCH_TIMEOUT, 0, 1}});

    // Topic options reach domain 7 as the row subscribe's reach the seam (rung 7, P5).
    const fl_topic_options options = {{nullptr, 0}, /*max_payload_bytes=*/1};
    const OwnedTopic row_topic("d9s5-options", "row");
    const OwnedTopic batch_topic("d9s5-options", "batched");
    fx.DeclareTopic(row_topic.topic(), published);
    fx.DeclareTopic(batch_topic.topic(), published);
    fl_error row_err = {};
    fl_error batch_err = {};
    uint64_t row_id = 0;
    uint64_t batch_id = 0;
    fl_schema_arrival* row_arrival = nullptr;
    fl_schema_arrival* batch_arrival = nullptr;
    const fl_status row_status = fl_subscriber_subscribe_with_options(
        fx.subscriber(), row_topic.topic(), &RowSink::OnRow, &rows, &options, &row_id, &row_arrival,
        &row_err);
    const fl_status batch_status = fl_subscriber_subscribe_batches(
        fx.subscriber(), batch_topic.topic(), 100, INT64_MAX, &options, &BatchSink::OnBatch,
        &sinks.front(), &batch_id, &batch_arrival, &batch_err);
    EXPECT_NE(row_status, FL_OK) << "precondition: the row subscribe refuses this option";
    EXPECT_EQ(batch_status, row_status);
    EXPECT_EQ(MessageOf(batch_err), MessageOf(row_err));
    if (row_status == FL_OK) fl_schema_arrival_dispose(row_arrival);
    if (batch_status == FL_OK) fl_schema_arrival_dispose(batch_arrival);
    fl_error_dispose(&row_err);
    fl_error_dispose(&batch_err);
}

// ---------------------------------------------------------------------------
// The server build only (5b)
// ---------------------------------------------------------------------------

/// How long the test thread waits for an event that is owed (one-sided), and how long a held
/// closing delivery waits for its closer to return.
constexpr milliseconds kAwait{1000};

class Latch {
   public:
    void Open() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            open_ = true;
        }
        cv_.notify_all();
    }
    bool WaitFor(milliseconds limit) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, limit, [&] { return open_; });
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool open_ = false;
};

/// One raw 4-byte row, through the raw route: on an undeclared topic it has no schema, so domain 7
/// has nothing to decode it with.
void PublishRaw(fl_publisher* publisher, fl_topic topic) {
    struct Writer {
        static size_t Row(void*, uint8_t* dst, size_t room) {
            if (room < 4) return 0;
            std::memset(dst, 0, 4);
            return 4;
        }
    };
    fl_error err = {};
    EXPECT_EQ(fl_publisher_publish_raw(publisher, topic, &Writer::Row, nullptr, 4, nullptr, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);
}

/// One cell of F1: its own subscriber, its own batched subscription, and the closer that ends it,
/// run from the test thread, from inside the subscription's own callback, or from a row delivery
/// on another topic of the same subscriber. The callback records into `sink` and never asserts.
class CloseCell {
   public:
    enum class By { kUnsubscribe, kDestroy };
    enum class From { kOutside, kInside, kRowDelivery };
    /// kQueued is the control: a full window queued and one row pending when closed from inside.
    enum class State { kDecoded, kUndecoded, kNoRow, kQueued };

    CloseCell(By by, From from, State state) : by_(by), from_(from), state_(state) {}

    static void OnBatch(void* ctx, const fl_batch* batch) {
        auto* self = static_cast<CloseCell*>(ctx);
        // From inside or a row delivery the close returns first: a closing is held until it has,
        // so a close that waited for its closing is seen at once (bounded by kAwait).
        if (batch->reason == FL_BATCH_CLOSING && self->from_ != From::kOutside) {
            static_cast<void>(self->closed_.WaitFor(kAwait));
        }
        BatchSink::OnBatch(&self->sink, batch);
        if (self->from_ == From::kInside && !self->fired_.exchange(true)) {
            if (self->state_ == State::kQueued) static_cast<void>(self->published.WaitFor(kAwait));
            self->Close();
        }
    }
    static void OnRow(void* ctx, uint64_t, const uint8_t*, size_t, const fl_schema*,
                      const fl_attachments*) {
        auto* self = static_cast<CloseCell*>(ctx);
        if (!self->fired_.exchange(true)) self->Close();
    }

    void Close() {
        if (by_ == By::kDestroy) {
            destroyed = true;
            fl_subscriber_destroy(subscriber);
        } else {
            fl_error err = {};
            close_status = fl_subscriber_unsubscribe_batches(subscriber, id, &err);
            fl_error_dispose(&err);
        }
        at_return = sink.Snapshot().size();
        closed_.Open();
    }
    bool WaitClosed(milliseconds limit) { return closed_.WaitFor(limit); }

    std::string Name() const {
        return std::string(by_ == By::kDestroy ? "fl_subscriber_destroy" : "unsubscribe_batches") +
               (from_ == From::kOutside  ? " from outside"
                : from_ == From::kInside ? " inside its own callback"
                                         : " from a row delivery (d)") +
               (state_ == State::kDecoded     ? ", idle after a decoded row"
                : state_ == State::kUndecoded ? ", idle after an undecodable row"
                : state_ == State::kNoRow     ? ", before any row"
                                              : ", a window queued and a row pending (control)");
    }

    By by() const { return by_; }
    From from() const { return from_; }
    State state() const { return state_; }

    BatchSink sink;
    Latch published;  // kQueued: every row has reached domain 7
    fl_subscriber* subscriber = nullptr;
    uint64_t id = 0;
    std::atomic<bool> destroyed{false};
    std::atomic<size_t> at_return{SIZE_MAX};
    std::atomic<fl_status> close_status{FL_OK};

   private:
    const By by_;
    const From from_;
    const State state_;
    std::atomic<bool> fired_{false};
    Latch closed_;
};

/// F1, rung 6 (d9 L15, L14; d7 L6, L7, L9 for closing; ruling 2026-11-14, "every close ends with
/// one closing delivery"). Each closer, from outside, from inside the subscription's own callback
/// and (unsubscribe only) from a row delivery on another topic of the same subscriber (DEBT-2),
/// on an idle window after a decoded row (1 row, max_rows 1), after an undecodable one (1 raw row
/// on an undeclared topic, 10 ms timeout), or before any row (declared topic). Each ends with
/// EXACTLY ONE CLOSING, last, 0 dropped. After a decoded row it is an empty table of the topic's
/// schema (array and schema non-NULL, num_rows 0); otherwise there is no array. From outside the
/// close returns after it (d7 L9); from inside or (d) the close returns first and it follows
/// within kAwait. Controls, green before: from inside with a full window queued and one row
/// pending, both closers give ROW_LIMIT, ROW_LIMIT, CLOSING(1). One shared 2 s window at the end:
/// a row to every cell's topic reaches no cell, while a live subscription gets its ROW_LIMIT.
TEST(Batches, EveryCloseEndsWithExactlyOneClosingDelivery) {
    using By = CloseCell::By;
    using From = CloseCell::From;
    using State = CloseCell::State;
    std::deque<CloseCell> cells;  // they and their sinks outlive the fixture
    std::deque<OwnedTopic> topics;
    BatchSink control;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/100, /*count=*/5);

    for (const By by : {By::kUnsubscribe, By::kDestroy}) {
        for (const State s : {State::kDecoded, State::kUndecoded, State::kNoRow, State::kQueued}) {
            if (s != State::kQueued) cells.emplace_back(by, From::kOutside, s);
            if (s != State::kNoRow) cells.emplace_back(by, From::kInside, s);
            if (by == By::kUnsubscribe && s != State::kQueued)
                cells.emplace_back(by, From::kRowDelivery, s);
        }
    }
    std::vector<size_t> counts;
    for (size_t i = 0; i < cells.size(); ++i) {
        CloseCell& c = cells[i];
        SCOPED_TRACE(c.Name());
        fl_error err = {};
        EXPECT_EQ(fl_subscriber_create(fx.provider(), &c.subscriber, &err), FL_OK)
            << MessageOf(err);
        topics.emplace_back("d9s5b-close", std::to_string(i));
        const fl_topic topic = topics.back().topic();
        if (c.state() != State::kUndecoded) fx.DeclareTopic(topic, published);
        const bool undecoded = c.state() == State::kUndecoded;
        fl_schema_arrival* arrival = nullptr;
        EXPECT_EQ(fl_subscriber_subscribe_batches(c.subscriber, topic,
                                                  undecoded                     ? 100
                                                  : c.state() == State::kQueued ? 2
                                                                                : 1,
                                                  undecoded ? 10 : INT64_MAX, nullptr,
                                                  &CloseCell::OnBatch, &c, &c.id, &arrival, &err),
                  FL_OK)
            << MessageOf(err);
        if (arrival != nullptr) fl_schema_arrival_dispose(arrival);
        if (c.from() == From::kRowDelivery) {
            const OwnedTopic& other = topics.emplace_back("d9s5b-close-row", std::to_string(i));
            fx.DeclareTopic(other.topic(), published);
            uint64_t row_id = 0;
            fl_schema_arrival* row_arrival = nullptr;
            EXPECT_EQ(fl_subscriber_subscribe(c.subscriber, other.topic(), &CloseCell::OnRow, &c,
                                              &row_id, &row_arrival, &err),
                      FL_OK)
                << MessageOf(err);
            if (row_arrival != nullptr) fl_schema_arrival_dispose(row_arrival);
        }
        fl_error_dispose(&err);

        // The trigger, and how many deliveries come before the closing.
        size_t before = 1;
        if (c.state() == State::kDecoded) fx.Publish(topic, published, 0, 1);
        if (c.state() == State::kUndecoded) PublishRaw(fx.publisher(), topic);
        if (c.state() == State::kNoRow) before = 0;
        if (c.state() == State::kQueued) {
            fx.Publish(topic, published, 0, 5);
            c.published.Open();
            before = 2;
        }
        if (c.from() != From::kInside) {
            EXPECT_TRUE(c.sink.WaitForDeliveries(before, kAwait))
                << "the trigger was not delivered";
            if (c.from() == From::kOutside) {
                c.Close();
            } else {
                fx.Publish(topics.back().topic(), published, 0, 1);  // the row delivery closes
            }
        }
        EXPECT_TRUE(c.WaitClosed(kAwait)) << "the close did not return";
        EXPECT_TRUE(c.sink.WaitForDeliveries(before + 1, kAwait)) << "no closing delivery arrived";
        const std::vector<Seen> seen = c.sink.Snapshot();
        counts.push_back(seen.size());

        EXPECT_EQ(c.close_status.load(), FL_OK);
        const size_t want_at_return = c.from() == From::kOutside  ? before + 1
                                      : c.from() == From::kInside ? 1
                                                                  : before;
        EXPECT_EQ(c.at_return.load(), want_at_return)
            << (c.from() == From::kOutside ? "the close returned before its closing (d7 L9)"
                                           : "the close waited for its closing (d9 L15)");
        if (c.state() == State::kQueued) {
            ExpectWindows(
                seen, published,
                {{FL_BATCH_ROW_LIMIT, 0, 2}, {FL_BATCH_ROW_LIMIT, 2, 2}, {FL_BATCH_CLOSING, 4, 1}});
            continue;
        }
        if (seen.size() != before + 1) {
            ADD_FAILURE() << "the trigger, then exactly one closing delivery: got " << seen.size()
                          << " deliveries";
            continue;
        }
        if (c.state() == State::kDecoded) {
            EXPECT_EQ(seen[0].reason, FL_BATCH_ROW_LIMIT);
            EXPECT_EQ(seen[0].num_rows, 1);
        } else if (c.state() == State::kUndecoded) {
            EXPECT_EQ(seen[0].reason, FL_BATCH_TIMEOUT);
            EXPECT_FALSE(seen[0].had_array);
            EXPECT_EQ(seen[0].rows_dropped, 1);
        }
        const Seen& closing = seen.back();
        EXPECT_EQ(closing.reason, FL_BATCH_CLOSING);
        EXPECT_EQ(closing.rows_dropped, 0);
        EXPECT_EQ(closing.num_rows, 0);
        EXPECT_FALSE(closing.had_attachments) << "no rows, so no attachments list (d9 L14)";
        if (c.state() == State::kDecoded) {
            EXPECT_TRUE(closing.had_array && closing.had_schema)
                << "after a decoded row the closing is an empty table, not \"no batch\" (d7 L7)";
            EXPECT_TRUE(closing.imported) << closing.import_error;
            if (closing.imported) {
                EXPECT_EQ(closing.batch->num_rows(), 0);
                EXPECT_TRUE(closing.batch->schema()->Equals(*published.batch().schema()))
                    << closing.batch->schema()->ToString();
            }
        } else {
            EXPECT_FALSE(closing.had_array) << "no decoder: the closing is \"no batch\" (d7 L7)";
            EXPECT_FALSE(closing.had_schema);
        }
    }

    // The shared window: a row to every cell's topic, and one to a live subscription.
    const OwnedTopic live("d9s5b-close", "live");
    fx.DeclareTopic(live.topic(), published);
    SubscribeBatches(fx.subscriber(), live.topic(), 1, INT64_MAX, control);
    for (size_t i = 0, t = 0; i < cells.size(); ++i, ++t) {
        if (cells[i].state() == State::kUndecoded) {
            PublishRaw(fx.publisher(), topics[t].topic());
        } else {
            fx.Publish(topics[t].topic(), published, 0, 1);
        }
        if (cells[i].from() == From::kRowDelivery) ++t;  // skip the cell's row topic
    }
    fx.Publish(live.topic(), published, 0, 1);
    EXPECT_TRUE(control.WaitForDeliveries(1, seconds(2))) << "the live subscription got nothing";
    std::this_thread::sleep_for(seconds(2));
    for (size_t i = 0; i < cells.size(); ++i) {
        EXPECT_EQ(cells[i].sink.Snapshot().size(), counts[i])
            << cells[i].Name() << ": a delivery after its closing";
    }
    ExpectWindows(control.Snapshot(), published, {{FL_BATCH_ROW_LIMIT, 0, 1}});
    for (CloseCell& c : cells) {
        if (!c.destroyed) fl_subscriber_destroy(c.subscriber);
    }
}

/// The flood's live subscription: counts, and keeps nothing.
struct CountingSink {
    static void OnBatch(void* ctx, const fl_batch*) { ++static_cast<CountingSink*>(ctx)->count; }
    std::atomic<int64_t> count{0};
};

/// F2 (ruling 2026-11-14, "a refused batched subscribe never delivers"; d7 L1's refused clause).
/// A publisher floods topic `busy`, where a batched subscription with no topic options is live.
/// 200 batched subscribes to `busy` with a conflicting profile, max_rows 1, each with its own
/// sink as `ctx`, half from the test thread and half from inside a row delivery on another topic
/// of the same subscriber: each answers FL_INVALID_ARGUMENT, and no refused sink is ever called,
/// through a final 2 s window. Domain 3 publishes a subscription before it refuses it, so rows
/// the flood delivers in between reach the refused one's batcher; whether any do is a race, so
/// the number of refused sinks called is printed, red or green. Control: during the same flood, a
/// subscribe with the matching (no) options gets a ROW_LIMIT within 2 s, so the flood does reach a
/// newly published subscription and a silent refused sink means something.
TEST(Batches, ARefusedBatchedSubscribeNeverDelivers) {
    constexpr size_t kAttempts = 200;
    std::deque<BatchSink> refused(kAttempts);  // one ctx per attempt; they outlive the fixture
    BatchSink control;
    CountingSink live;
    struct InsideAttempt {
        std::function<void()> attempt;
        static void OnRow(void* ctx, uint64_t, const uint8_t*, size_t, const fl_schema*,
                          const fl_attachments*) {
            static_cast<InsideAttempt*>(ctx)->attempt();
        }
    } inside;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/1, /*count=*/1);
    const OwnedTopic busy("d9s5b-refused", "busy");
    const OwnedTopic other("d9s5b-refused", "other");
    fx.DeclareTopic(busy.topic(), published);
    fx.DeclareTopic(other.topic(), published);

    fl_error err = {};
    uint64_t live_id = 0;
    fl_schema_arrival* arrival = nullptr;
    ASSERT_EQ(fl_subscriber_subscribe_batches(fx.subscriber(), busy.topic(), INT64_MAX, INT64_MAX,
                                              nullptr, &CountingSink::OnBatch, &live, &live_id,
                                              &arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_schema_arrival_dispose(arrival);

    const fl_topic_options conflicting = {Str("d9s5b-conflict"), 0};
    std::vector<fl_status> status(kAttempts, FL_OK);
    std::vector<char> ran(kAttempts, 0);
    size_t next = 0;
    const auto attempt = [&] {
        const size_t i = next;
        fl_error attempt_err = {};
        uint64_t id = 0;
        fl_schema_arrival* attempt_arrival = nullptr;
        status[i] = fl_subscriber_subscribe_batches(fx.subscriber(), busy.topic(), 1, INT64_MAX,
                                                    &conflicting, &BatchSink::OnBatch, &refused[i],
                                                    &id, &attempt_arrival, &attempt_err);
        if (status[i] == FL_OK) fl_schema_arrival_dispose(attempt_arrival);
        fl_error_dispose(&attempt_err);
        ran[i] = 1;
    };
    inside.attempt = attempt;
    uint64_t row_id = 0;
    ASSERT_EQ(fl_subscriber_subscribe(fx.subscriber(), other.topic(), &InsideAttempt::OnRow,
                                      &inside, &row_id, &arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_schema_arrival_dispose(arrival);
    fl_error_dispose(&err);

    std::atomic<bool> stop{false};
    std::atomic<int64_t> flood_failures{0};
    std::atomic<int64_t> flooded{0};
    std::thread flood([&] {
        while (!stop.load()) {
            ++flooded;
            fl_error flood_err = {};
            if (fl_publisher_publish_rows(fx.publisher(), busy.topic(), published.rows(), 0, 1,
                                          nullptr, &flood_err) != FL_OK) {
                ++flood_failures;
            }
            fl_error_dispose(&flood_err);
        }
    });
    for (next = 0; next < kAttempts; ++next) {
        if (next % 2 == 0) {
            attempt();
        } else {
            fx.Publish(other.topic(), published, 0, 1);  // the row delivery makes the attempt
        }
    }
    const uint64_t control_id =
        SubscribeBatches(fx.subscriber(), busy.topic(), 1, INT64_MAX, control);
    const bool control_saw = control.WaitForDeliveries(1, seconds(2));
    stop = true;
    flood.join();
    CloseBatches(fx.subscriber(), control_id, FL_OK);
    std::this_thread::sleep_for(seconds(2));

    size_t called_outside = 0;
    size_t called_inside = 0;
    std::string first_called;  // what the first refused sink that was called received
    for (size_t i = 0; i < kAttempts; ++i) {
        SCOPED_TRACE("attempt " + std::to_string(i) + (i % 2 == 0 ? " (outside)" : " (inside)"));
        EXPECT_TRUE(ran[i]) << "the attempt was not made";
        EXPECT_EQ(status[i], FL_INVALID_ARGUMENT);
        const std::vector<Seen> got = refused[i].Snapshot();
        if (got.empty()) continue;
        ++(i % 2 == 0 ? called_outside : called_inside);
        if (!first_called.empty()) continue;
        first_called = "attempt " + std::to_string(i) + ":";
        for (const Seen& s : got) {
            first_called += " reason " + std::to_string(s.reason) + ", rows " +
                            std::to_string(s.num_rows) + ", dropped " +
                            std::to_string(s.rows_dropped) + ";";
        }
    }
    std::cerr << "[ F2 ] refused sinks called: " << called_outside + called_inside << " of "
              << kAttempts << " (outside " << called_outside << ", inside " << called_inside
              << "); rows flooded " << flooded.load() << "; " << first_called << "\n";
    EXPECT_EQ(called_outside + called_inside, 0U) << "a refused batched subscribe delivered";
    EXPECT_EQ(flood_failures.load(), 0);
    EXPECT_TRUE(control_saw) << "control: the flood never reached a newly published subscription";
    const std::vector<Seen> control_seen = control.Snapshot();
    ASSERT_FALSE(control_seen.empty());
    ExpectWindows({control_seen.front()}, published, {{FL_BATCH_ROW_LIMIT, 0, 1}});
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
        ExpectRefusedAsEdge(status, err);
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

/// d9 L12, L16, BC-10 (guard e), for the batched close; D9S-4 B1 (bit 63). ONE body, one answer
/// per artefact. A row subscription on another topic is live throughout, and must still deliver
/// at the end whatever the close was handed: its own id, bare and tagged, on the edge; on the
/// server the bare raw id, the number it shares with the batched subscription.
///
///   * edge and probe: every call, a NULL subscriber included, is the edge refusal.
///   * server (the control): an untagged id and a NULL subscriber are FL_INVALID_ARGUMENT and
///     close nothing; the tagged id, with one row pending, is FL_OK and its CLOSING has arrived
///     when the call returns (d7 L9).
TEST(Batches, TheBuildAnswersABatchedCloseAsItAnswersTheQuery) {
    BatchSink sink;
    RowSink rows;
    BatchFixture fx;
    PublishedRows published(/*first_seq=*/7, /*count=*/1);
    const OwnedTopic batched("d9s5", "close-truth-table");
    const OwnedTopic row_topic("d9s5", "close-row");
    fx.DeclareTopic(batched.topic(), published);
    fx.DeclareTopic(row_topic.topic(), published);

    fl_error err = {};
    uint64_t row_id = 0;
    fl_schema_arrival* row_arrival = nullptr;
    ASSERT_EQ(fl_subscriber_subscribe(fx.subscriber(), row_topic.topic(), &RowSink::OnRow, &rows,
                                      &row_id, &row_arrival, &err),
              FL_OK)
        << MessageOf(err);
    fl_error_dispose(&err);
    fl_schema_arrival_dispose(row_arrival);

    fl_error null_err = {};
    const fl_status null_status = fl_subscriber_unsubscribe_batches(nullptr, row_id, &null_err);

    if (kServerBuild) {
        const uint64_t id = SubscribeBatches(fx.subscriber(), batched.topic(), 100, INT64_MAX, sink);
        fx.Publish(batched.topic(), published, 0, 1);
        for (const uint64_t bare : {row_id, id & ~kBatchedIdTag}) {
            SCOPED_TRACE("untagged id " + std::to_string(bare));
            CloseBatches(fx.subscriber(), bare, FL_INVALID_ARGUMENT);
            EXPECT_TRUE(sink.Snapshot().empty()) << "an untagged id closed a batched subscription";
        }
        CloseBatches(fx.subscriber(), id, FL_OK);
        ExpectWindows(sink.Snapshot(), published, {{FL_BATCH_CLOSING, 0, 1}});

        EXPECT_EQ(null_status, FL_INVALID_ARGUMENT) << MessageOf(null_err);
        EXPECT_EQ(null_err.origin, FL_ORIGIN_SEAM);
    } else {
        for (const uint64_t any : {row_id, row_id | kBatchedIdTag}) {
            SCOPED_TRACE("id " + std::to_string(any));
            fl_error edge_err = {};
            ExpectRefusedAsEdge(fl_subscriber_unsubscribe_batches(fx.subscriber(), any, &edge_err),
                                edge_err);
            fl_error_dispose(&edge_err);
        }
        SCOPED_TRACE("a NULL subscriber: the door reads the build before any argument");
        ExpectRefusedAsEdge(null_status, null_err);
    }

    fx.Publish(row_topic.topic(), published, 0, 1);
    EXPECT_TRUE(rows.WaitFor(1, seconds(2))) << "the row subscription stopped delivering";
    fl_error_dispose(&null_err);
}

}  // namespace
