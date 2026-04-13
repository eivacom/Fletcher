#include <gtest/gtest.h>

#include <arrow/api.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "row_codec.hpp"
#include "generic_row_batcher.hpp"
#include "sqlite_wal.hpp"

using namespace fletcher;

namespace {

auto SimpleSchema() {
    return arrow::schema({arrow::field("id",   arrow::int32(), false),
                          arrow::field("name", arrow::utf8(),  true)});
}

EncodedRow MakeRow(RowCodec& codec, int32_t id, const char* name) {
    return codec.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(id),
        std::make_shared<arrow::StringScalar>(name)
    });
}

// Subclass that tracks OnBatchFlushSucceeded / OnBatchFlushFailed invocations.
// Counters are references to atomics that outlive the batcher, so they can be
// safely read after the batcher's destructor has joined all flush threads.
class TrackedBatcher : public GenericRowBatcher {
 public:
    TrackedBatcher(std::shared_ptr<arrow::Schema> s, WriteAheadLog& w,
                   int64_t batch_size, FlushCallback cb,
                   std::atomic<int>& succeeded, std::atomic<int>& failed)
        : GenericRowBatcher(std::move(s), w, batch_size, std::move(cb))
        , succeeded_(succeeded)
        , failed_(failed)
    {}

    // Wait while our vtable is still intact so the overrides are called.
    ~TrackedBatcher() override { WaitForPendingFlushes(); }

 protected:
    void OnBatchFlushSucceeded() override { succeeded_++; }
    void OnBatchFlushFailed()    override { failed_++;    }

 private:
    std::atomic<int>& succeeded_;
    std::atomic<int>& failed_;
};

}  // namespace

// ---------------------------------------------------------------------------
// row_count behaviour
// ---------------------------------------------------------------------------

TEST(BatcherTest, RowCountStartsAtZero) {
    auto schema = SimpleSchema();
    SQLiteWAL wal(":memory:");
    GenericRowBatcher batcher(schema, wal, 10, [](auto) { return true; });
    EXPECT_EQ(batcher.row_count(), 0);
}

TEST(BatcherTest, RowCountIncrementsWithEachAppend) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    GenericRowBatcher batcher(schema, wal, 10, [](auto) { return true; });

    batcher.Append(MakeRow(codec, 1, "a"));
    EXPECT_EQ(batcher.row_count(), 1);

    batcher.Append(MakeRow(codec, 2, "b"));
    EXPECT_EQ(batcher.row_count(), 2);
}

TEST(BatcherTest, RowCountResetsSynchronouslyAfterFlush) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    GenericRowBatcher batcher(schema, wal, 3, [](auto) { return true; });

    batcher.Append(MakeRow(codec, 1, "a"));
    batcher.Append(MakeRow(codec, 2, "b"));
    batcher.Append(MakeRow(codec, 3, "c"));  // triggers flush

    // row_count_ is reset to 0 under the lock before the async dispatch returns
    EXPECT_EQ(batcher.row_count(), 0);
}

// ---------------------------------------------------------------------------
// Flush triggering
// ---------------------------------------------------------------------------

TEST(BatcherTest, DoesNotFlushBelowBatchSize) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> flush_count{0};
    {
        GenericRowBatcher batcher(schema, wal, 4,
            [&](auto) { flush_count++; return true; });
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
        batcher.Append(MakeRow(codec, 3, "c"));
    }  // destructor joins all pending futures
    EXPECT_EQ(flush_count.load(), 0);
}

TEST(BatcherTest, FlushesExactlyAtBatchSize) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> flush_count{0};
    {
        GenericRowBatcher batcher(schema, wal, 3,
            [&](auto) { flush_count++; return true; });
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
        batcher.Append(MakeRow(codec, 3, "c"));  // batch complete
    }
    EXPECT_EQ(flush_count.load(), 1);
}

TEST(BatcherTest, FlushesMultipleCompleteBatches) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> flush_count{0};
    {
        GenericRowBatcher batcher(schema, wal, 2,
            [&](auto) { flush_count++; return true; });
        for (int i = 0; i < 6; ++i)
            batcher.Append(MakeRow(codec, i, "x"));
    }
    EXPECT_EQ(flush_count.load(), 3);
}

TEST(BatcherTest, PartialBatchIsNotFlushedOnDestruction) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> flush_count{0};
    {
        GenericRowBatcher batcher(schema, wal, 4,
            [&](auto) { flush_count++; return true; });
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
        batcher.Append(MakeRow(codec, 3, "c"));
    }
    EXPECT_EQ(flush_count.load(), 0);
}

// ---------------------------------------------------------------------------
// Flushed table contents
// ---------------------------------------------------------------------------

TEST(BatcherTest, FlushedTableHasBatchSizeRows) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int64_t> flushed_rows{-1};
    {
        GenericRowBatcher batcher(schema, wal, 3,
            [&](std::shared_ptr<arrow::Table> t) {
                flushed_rows.store(t->num_rows());
                return true;
            });
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
        batcher.Append(MakeRow(codec, 3, "c"));
    }
    EXPECT_EQ(flushed_rows.load(), 3);
}

TEST(BatcherTest, FlushedTableSchemaMatchesInputSchema) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<bool> schema_matched{false};
    {
        GenericRowBatcher batcher(schema, wal, 2,
            [&](std::shared_ptr<arrow::Table> t) {
                schema_matched.store(t->schema()->Equals(*schema));
                return true;
            });
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
    }
    EXPECT_TRUE(schema_matched.load());
}

TEST(BatcherTest, FlushedTableDataMatchesAppendedRows) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");

    std::shared_ptr<arrow::Table> captured;
    std::mutex captured_mutex;
    {
        GenericRowBatcher batcher(schema, wal, 2,
            [&](std::shared_ptr<arrow::Table> t) {
                std::lock_guard<std::mutex> lk(captured_mutex);
                captured = t;
                return true;
            });
        batcher.Append(MakeRow(codec, 10, "hello"));
        batcher.Append(MakeRow(codec, 20, "world"));
    }  // destructor joins flush thread

    ASSERT_NE(captured, nullptr);
    ASSERT_EQ(captured->num_rows(), 2);

    auto id_col = std::static_pointer_cast<arrow::Int32Array>(
        captured->GetColumnByName("id")->chunk(0));
    EXPECT_EQ(id_col->Value(0), 10);
    EXPECT_EQ(id_col->Value(1), 20);

    auto name_col = std::static_pointer_cast<arrow::StringArray>(
        captured->GetColumnByName("name")->chunk(0));
    EXPECT_EQ(name_col->GetString(0), "hello");
    EXPECT_EQ(name_col->GetString(1), "world");
}

// ---------------------------------------------------------------------------
// Flush hooks
// ---------------------------------------------------------------------------

TEST(BatcherTest, OnBatchFlushSucceededCalledWhenCallbackReturnsTrue) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> succeeded{0}, failed{0};
    {
        TrackedBatcher batcher(schema, wal, 2, [](auto) { return true; },
                               succeeded, failed);
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
    }
    EXPECT_EQ(succeeded.load(), 1);
    EXPECT_EQ(failed.load(),    0);
}

TEST(BatcherTest, OnBatchFlushFailedCalledWhenCallbackReturnsFalse) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> succeeded{0}, failed{0};
    {
        TrackedBatcher batcher(schema, wal, 2, [](auto) { return false; },
                               succeeded, failed);
        batcher.Append(MakeRow(codec, 1, "a"));
        batcher.Append(MakeRow(codec, 2, "b"));
    }
    EXPECT_EQ(succeeded.load(), 0);
    EXPECT_EQ(failed.load(),    1);
}

TEST(BatcherTest, HooksCalledOncePerBatch) {
    auto schema = SimpleSchema();
    RowCodec codec(schema);
    SQLiteWAL wal(":memory:");
    std::atomic<int> succeeded{0}, failed{0};
    {
        TrackedBatcher batcher(schema, wal, 2, [](auto) { return true; },
                               succeeded, failed);
        for (int i = 0; i < 4; ++i)
            batcher.Append(MakeRow(codec, i, "x"));
    }
    EXPECT_EQ(succeeded.load(), 2);
    EXPECT_EQ(failed.load(),    0);
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(BatcherTest, AppendThrowsOnSchemaMismatchedRow) {
    auto schema_a = SimpleSchema();
    auto schema_b = arrow::schema({arrow::field("x", arrow::float64())});
    RowCodec codec_b(schema_b);
    SQLiteWAL wal(":memory:");
    GenericRowBatcher batcher(schema_a, wal, 10, [](auto) { return true; });

    auto foreign_row = codec_b.EncodeRow({std::make_shared<arrow::DoubleScalar>(3.14)});
    EXPECT_THROW(batcher.Append(foreign_row), std::invalid_argument);
}

TEST(BatcherTest, AppendThrowsOnTruncatedBuffer) {
    auto schema = SimpleSchema();
    SQLiteWAL wal(":memory:");
    GenericRowBatcher batcher(schema, wal, 10, [](auto) { return true; });

    EncodedRow garbage{0x01, 0x02, 0x03};
    EXPECT_ANY_THROW(batcher.Append(garbage));
}
