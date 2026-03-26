#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<arrow::Scalar> RoundtripNull(
    const std::shared_ptr<arrow::DataType>& type)
{
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    arrow_row::RowCodec codec(schema);
    auto null    = arrow::MakeNullScalar(type);
    auto row     = codec.EncodeRow({null});
    auto decoded = codec.DecodeRow(row);
    REQUIRE(decoded.size() == 1);
    return decoded[0];
}

void CheckNull(const std::shared_ptr<arrow::DataType>& type) {
    auto d = RoundtripNull(type);
    CHECK(!d->is_valid);
    CHECK(d->type->Equals(*type));
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixed-width nulls
// ---------------------------------------------------------------------------

TEST_CASE("Null fixed-width scalars roundtrip") {
    CheckNull(arrow::boolean());
    CheckNull(arrow::int8());
    CheckNull(arrow::int16());
    CheckNull(arrow::int32());
    CheckNull(arrow::int64());
    CheckNull(arrow::uint8());
    CheckNull(arrow::uint16());
    CheckNull(arrow::uint32());
    CheckNull(arrow::uint64());
    CheckNull(arrow::float32());
    CheckNull(arrow::float64());
    CheckNull(arrow::float16());
    CheckNull(arrow::date32());
    CheckNull(arrow::date64());
    CheckNull(arrow::month_interval());
    CheckNull(arrow::day_time_interval());
    CheckNull(arrow::month_day_nano_interval());
}

TEST_CASE("Null timestamp roundtrip") {
    CheckNull(arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"));
}

TEST_CASE("Null time32 / time64 / duration roundtrip") {
    CheckNull(arrow::time32(arrow::TimeUnit::MILLI));
    CheckNull(arrow::time64(arrow::TimeUnit::NANO));
    CheckNull(arrow::duration(arrow::TimeUnit::SECOND));
}

TEST_CASE("Null fixed_size_binary roundtrip") {
    CheckNull(arrow::fixed_size_binary(8));
}

TEST_CASE("Null decimal roundtrip") {
    CheckNull(arrow::decimal128(10, 3));
    CheckNull(arrow::decimal256(38, 6));
}

// ---------------------------------------------------------------------------
// Variable-width nulls
// ---------------------------------------------------------------------------

TEST_CASE("Null string / binary scalars roundtrip") {
    CheckNull(arrow::utf8());
    CheckNull(arrow::large_utf8());
    CheckNull(arrow::binary());
    CheckNull(arrow::large_binary());
    CheckNull(arrow::utf8_view());
    CheckNull(arrow::binary_view());
}

// ---------------------------------------------------------------------------
// Recursive nulls
// ---------------------------------------------------------------------------

TEST_CASE("Null struct roundtrip") {
    auto type = arrow::struct_({arrow::field("x", arrow::int32()),
                                 arrow::field("y", arrow::utf8())});
    CheckNull(type);
}

TEST_CASE("Null list / large_list roundtrip") {
    CheckNull(arrow::list(arrow::int32()));
    CheckNull(arrow::large_list(arrow::utf8()));
}

TEST_CASE("Null fixed_size_list roundtrip") {
    CheckNull(arrow::fixed_size_list(arrow::float32(), 3));
}

TEST_CASE("Null map roundtrip") {
    CheckNull(arrow::map(arrow::utf8(), arrow::int32()));
}

// ---------------------------------------------------------------------------
// Multi-field schema: mix of null and non-null values
// ---------------------------------------------------------------------------

TEST_CASE("Multi-field row with some null values") {
    auto schema = arrow::schema({
        arrow::field("id",    arrow::int32(),  false),
        arrow::field("name",  arrow::utf8(),   true),
        arrow::field("score", arrow::float64(), true),
    });
    arrow_row::RowCodec codec(schema);

    std::vector<std::shared_ptr<arrow::Scalar>> in = {
        std::make_shared<arrow::Int32Scalar>(7),
        arrow::MakeNullScalar(arrow::utf8()),
        std::make_shared<arrow::DoubleScalar>(9.5),
    };
    auto row     = codec.EncodeRow(in);
    auto decoded = codec.DecodeRow(row);

    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0]->Equals(*in[0]));
    CHECK(!decoded[1]->is_valid);
    CHECK(decoded[2]->Equals(*in[2]));
}
