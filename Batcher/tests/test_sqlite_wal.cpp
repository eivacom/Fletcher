#include <gtest/gtest.h>

#include <arrow/api.h>

#include "row_codec.hpp"
#include "sqlite_wal.hpp"

using namespace fletcher;

namespace {

auto TwoFieldSchema() {
    return arrow::schema({arrow::field("id",   arrow::int32(), false),
                          arrow::field("name", arrow::utf8(),  true)});
}

// Encode a row with the given id and name (nullptr = null name).
EncodedRow MakeRow(RowCodec& codec, int32_t id, const char* name) {
    std::shared_ptr<arrow::Scalar> name_scalar =
        name ? std::make_shared<arrow::StringScalar>(name)
             : arrow::MakeNullScalar(arrow::utf8());
    return codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(id), name_scalar});
}

int32_t GetInt(const std::shared_ptr<arrow::Table>& t, const std::string& col, int row) {
    return std::static_pointer_cast<arrow::Int32Array>(
               t->GetColumnByName(col)->chunk(0))->Value(row);
}

std::string GetStr(const std::shared_ptr<arrow::Table>& t, const std::string& col, int row) {
    return std::static_pointer_cast<arrow::StringArray>(
               t->GetColumnByName(col)->chunk(0))->GetString(row);
}

bool IsNull(const std::shared_ptr<arrow::Table>& t, const std::string& col, int row) {
    return t->GetColumnByName(col)->chunk(0)->IsNull(row);
}

}  // namespace

TEST(SqliteWalTest, OpensInMemoryDatabase) {
    EXPECT_NO_THROW(SQLiteWAL(":memory:"));
}

TEST(SqliteWalTest, CreateLogReturnsValidHandle) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    auto handle = wal.CreateLog(*schema, {});
    EXPECT_NE(handle, nullptr);
}

TEST(SqliteWalTest, ToTableOnEmptyLogReturnsZeroRowTable) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    auto handle = wal.CreateLog(*schema, {});
    auto table  = handle->ToTable();
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->num_rows(), 0);
    EXPECT_EQ(table->num_columns(), 2);
}

TEST(SqliteWalTest, SingleRowRoundtrip) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    auto handle = wal.CreateLog(*schema, {});

    handle->Log(MakeRow(codec, 42, "hello"));

    auto table = handle->ToTable();
    ASSERT_EQ(table->num_rows(), 1);
    EXPECT_EQ(GetInt(table, "id",   0), 42);
    EXPECT_EQ(GetStr(table, "name", 0), "hello");
}

TEST(SqliteWalTest, MultipleRowsPreserveInsertionOrder) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    auto handle = wal.CreateLog(*schema, {});

    handle->Log(MakeRow(codec, 1, "alpha"));
    handle->Log(MakeRow(codec, 2, "beta"));
    handle->Log(MakeRow(codec, 3, "gamma"));

    auto table = handle->ToTable();
    ASSERT_EQ(table->num_rows(), 3);
    EXPECT_EQ(GetInt(table, "id", 0), 1);
    EXPECT_EQ(GetInt(table, "id", 1), 2);
    EXPECT_EQ(GetInt(table, "id", 2), 3);
    EXPECT_EQ(GetStr(table, "name", 0), "alpha");
    EXPECT_EQ(GetStr(table, "name", 1), "beta");
    EXPECT_EQ(GetStr(table, "name", 2), "gamma");
}

TEST(SqliteWalTest, NullValuesRoundTripCorrectly) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    auto handle = wal.CreateLog(*schema, {});

    handle->Log(MakeRow(codec, 1, nullptr));  // null name
    handle->Log(MakeRow(codec, 2, "present"));

    auto table = handle->ToTable();
    ASSERT_EQ(table->num_rows(), 2);
    EXPECT_TRUE(IsNull(table, "name", 0));
    EXPECT_FALSE(IsNull(table, "name", 1));
    EXPECT_EQ(GetStr(table, "name", 1), "present");
}

TEST(SqliteWalTest, KeyColumnsOrdersToTableResults) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    // key_columns = {0} means ORDER BY id
    auto handle = wal.CreateLog(*schema, {0});

    handle->Log(MakeRow(codec, 30, "c"));
    handle->Log(MakeRow(codec, 10, "a"));
    handle->Log(MakeRow(codec, 20, "b"));

    auto table = handle->ToTable();
    ASSERT_EQ(table->num_rows(), 3);
    EXPECT_EQ(GetInt(table, "id", 0), 10);
    EXPECT_EQ(GetInt(table, "id", 1), 20);
    EXPECT_EQ(GetInt(table, "id", 2), 30);
    EXPECT_EQ(GetStr(table, "name", 0), "a");
    EXPECT_EQ(GetStr(table, "name", 1), "b");
    EXPECT_EQ(GetStr(table, "name", 2), "c");
}

TEST(SqliteWalTest, MultipleLogsAreIndependent) {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);

    auto h1 = wal.CreateLog(*schema, {});
    auto h2 = wal.CreateLog(*schema, {});

    h1->Log(MakeRow(codec, 1, "one"));
    h2->Log(MakeRow(codec, 2, "two"));
    h2->Log(MakeRow(codec, 3, "three"));

    auto t1 = h1->ToTable();
    auto t2 = h2->ToTable();

    EXPECT_EQ(t1->num_rows(), 1);
    EXPECT_EQ(t2->num_rows(), 2);
    EXPECT_EQ(GetInt(t1, "id", 0), 1);
    EXPECT_EQ(GetInt(t2, "id", 0), 2);
    EXPECT_EQ(GetInt(t2, "id", 1), 3);
}
