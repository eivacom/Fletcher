#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"
#include "sqlite_wal.hpp"

using namespace arrow_row;

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

TEST_CASE("SQLiteWAL opens in-memory database") {
    CHECK_NOTHROW(SQLiteWAL(":memory:"));
}

TEST_CASE("SQLiteWAL::CreateLog returns a valid handle") {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    auto handle = wal.CreateLog(*schema, {});
    CHECK(handle != nullptr);
}

TEST_CASE("SQLiteLogHandle ToTable on empty log returns zero-row table") {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    auto handle = wal.CreateLog(*schema, {});
    auto table  = handle->ToTable();
    REQUIRE(table != nullptr);
    CHECK(table->num_rows() == 0);
    CHECK(table->num_columns() == 2);
}

TEST_CASE("SQLiteLogHandle single row roundtrip") {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    auto handle = wal.CreateLog(*schema, {});

    handle->Log(MakeRow(codec, 42, "hello"));

    auto table = handle->ToTable();
    REQUIRE(table->num_rows() == 1);
    CHECK(GetInt(table, "id",   0) == 42);
    CHECK(GetStr(table, "name", 0) == "hello");
}

TEST_CASE("SQLiteLogHandle multiple rows preserve insertion order") {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    auto handle = wal.CreateLog(*schema, {});

    handle->Log(MakeRow(codec, 1, "alpha"));
    handle->Log(MakeRow(codec, 2, "beta"));
    handle->Log(MakeRow(codec, 3, "gamma"));

    auto table = handle->ToTable();
    REQUIRE(table->num_rows() == 3);
    CHECK(GetInt(table, "id", 0) == 1);
    CHECK(GetInt(table, "id", 1) == 2);
    CHECK(GetInt(table, "id", 2) == 3);
    CHECK(GetStr(table, "name", 0) == "alpha");
    CHECK(GetStr(table, "name", 1) == "beta");
    CHECK(GetStr(table, "name", 2) == "gamma");
}

TEST_CASE("SQLiteLogHandle null values round-trip correctly") {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    auto handle = wal.CreateLog(*schema, {});

    handle->Log(MakeRow(codec, 1, nullptr));  // null name
    handle->Log(MakeRow(codec, 2, "present"));

    auto table = handle->ToTable();
    REQUIRE(table->num_rows() == 2);
    CHECK(IsNull(table, "name", 0));
    CHECK_FALSE(IsNull(table, "name", 1));
    CHECK(GetStr(table, "name", 1) == "present");
}

TEST_CASE("SQLiteLogHandle key_columns orders ToTable results") {
    SQLiteWAL wal(":memory:");
    auto schema = TwoFieldSchema();
    RowCodec codec(schema);
    // key_columns = {0} means ORDER BY id
    auto handle = wal.CreateLog(*schema, {0});

    handle->Log(MakeRow(codec, 30, "c"));
    handle->Log(MakeRow(codec, 10, "a"));
    handle->Log(MakeRow(codec, 20, "b"));

    auto table = handle->ToTable();
    REQUIRE(table->num_rows() == 3);
    CHECK(GetInt(table, "id", 0) == 10);
    CHECK(GetInt(table, "id", 1) == 20);
    CHECK(GetInt(table, "id", 2) == 30);
    CHECK(GetStr(table, "name", 0) == "a");
    CHECK(GetStr(table, "name", 1) == "b");
    CHECK(GetStr(table, "name", 2) == "c");
}

TEST_CASE("SQLiteWAL multiple logs are independent") {
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

    CHECK(t1->num_rows() == 1);
    CHECK(t2->num_rows() == 2);
    CHECK(GetInt(t1, "id", 0) == 1);
    CHECK(GetInt(t2, "id", 0) == 2);
    CHECK(GetInt(t2, "id", 1) == 3);
}
