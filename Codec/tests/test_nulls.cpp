#include <gtest/gtest.h>

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
    fletcher::RowCodec codec(schema);
    auto null    = arrow::MakeNullScalar(type);
    auto row     = codec.EncodeRow({null});
    auto decoded = codec.DecodeRow(row);
    if (decoded.size() != 1) { ADD_FAILURE() << "Expected decoded.size() == 1"; return nullptr; }
    return decoded[0];
}

void CheckNull(const std::shared_ptr<arrow::DataType>& type) {
    auto d = RoundtripNull(type);
    ASSERT_NE(d, nullptr);
    EXPECT_FALSE(d->is_valid);
    EXPECT_TRUE(d->type->Equals(*type));
}

}  // namespace

// ---------------------------------------------------------------------------
// Fixed-width nulls
// ---------------------------------------------------------------------------

TEST(NullTest, FixedWidthScalarsRoundtrip) {
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

TEST(NullTest, TimestampRoundtrip) {
    CheckNull(arrow::timestamp(arrow::TimeUnit::MICRO, "UTC"));
}

TEST(NullTest, Time32Time64DurationRoundtrip) {
    CheckNull(arrow::time32(arrow::TimeUnit::MILLI));
    CheckNull(arrow::time64(arrow::TimeUnit::NANO));
    CheckNull(arrow::duration(arrow::TimeUnit::SECOND));
}

TEST(NullTest, FixedSizeBinaryRoundtrip) {
    CheckNull(arrow::fixed_size_binary(8));
}

TEST(NullTest, DecimalRoundtrip) {
    CheckNull(arrow::decimal128(10, 3));
    CheckNull(arrow::decimal256(38, 6));
}

// ---------------------------------------------------------------------------
// Variable-width nulls
// ---------------------------------------------------------------------------

TEST(NullTest, StringBinaryScalarsRoundtrip) {
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

TEST(NullTest, StructRoundtrip) {
    auto type = arrow::struct_({arrow::field("x", arrow::int32()),
                                 arrow::field("y", arrow::utf8())});
    CheckNull(type);
}

TEST(NullTest, ListLargeListRoundtrip) {
    CheckNull(arrow::list(arrow::int32()));
    CheckNull(arrow::large_list(arrow::utf8()));
}

TEST(NullTest, FixedSizeListRoundtrip) {
    CheckNull(arrow::fixed_size_list(arrow::float32(), 3));
}

TEST(NullTest, MapRoundtrip) {
    CheckNull(arrow::map(arrow::utf8(), arrow::int32()));
}

// ---------------------------------------------------------------------------
// Multi-field schema: mix of null and non-null values
// ---------------------------------------------------------------------------

TEST(NullTest, MultiFieldRowWithSomeNullValues) {
    auto schema = arrow::schema({
        arrow::field("id",    arrow::int32(),  false),
        arrow::field("name",  arrow::utf8(),   true),
        arrow::field("score", arrow::float64(), true),
    });
    fletcher::RowCodec codec(schema);

    fletcher::ArrowRow in = {
        std::make_shared<arrow::Int32Scalar>(7),
        arrow::MakeNullScalar(arrow::utf8()),
        std::make_shared<arrow::DoubleScalar>(9.5),
    };
    auto row     = codec.EncodeRow(in);
    auto decoded = codec.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_TRUE(decoded[0]->Equals(*in[0]));
    EXPECT_FALSE(decoded[1]->is_valid);
    EXPECT_TRUE(decoded[2]->Equals(*in[2]));
}
