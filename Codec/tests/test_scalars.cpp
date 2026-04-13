#include <gtest/gtest.h>

#include <arrow/api.h>

#include <cstring>
#include <string>

#include "row_codec.hpp"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

namespace {

// Encode one scalar through RowCodec and decode it back.
std::shared_ptr<arrow::Scalar> Roundtrip(
    const std::shared_ptr<arrow::DataType>& type,
    const std::shared_ptr<arrow::Scalar>&   in)
{
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    fletcher::RowCodec codec(schema);
    auto row     = codec.EncodeRow({in});
    auto decoded = codec.DecodeRow(row);
    if (decoded.size() != 1) { ADD_FAILURE() << "Expected decoded.size() == 1"; return nullptr; }
    return decoded[0];
}

#define CHECK_RT(type_expr, scalar_expr)                        \
    do {                                                        \
        auto _orig = (scalar_expr);                             \
        auto _dec  = Roundtrip((type_expr), _orig);             \
        ASSERT_NE(_dec, nullptr);                               \
        EXPECT_TRUE(_dec->Equals(*_orig));                      \
    } while (false)

}  // namespace

// ---------------------------------------------------------------------------
// Boolean
// ---------------------------------------------------------------------------

TEST(ScalarTest, BooleanRoundtrip) {
    CHECK_RT(arrow::boolean(), std::make_shared<arrow::BooleanScalar>(true));
    CHECK_RT(arrow::boolean(), std::make_shared<arrow::BooleanScalar>(false));
}

// ---------------------------------------------------------------------------
// Integers
// ---------------------------------------------------------------------------

TEST(ScalarTest, SignedIntegerRoundtrip) {
    CHECK_RT(arrow::int8(),  std::make_shared<arrow::Int8Scalar>(-128));
    CHECK_RT(arrow::int16(), std::make_shared<arrow::Int16Scalar>(32767));
    CHECK_RT(arrow::int32(), std::make_shared<arrow::Int32Scalar>(-1'000'000));
    CHECK_RT(arrow::int64(), std::make_shared<arrow::Int64Scalar>(9'223'372'036'854'775'807LL));
}

TEST(ScalarTest, UnsignedIntegerRoundtrip) {
    CHECK_RT(arrow::uint8(),  std::make_shared<arrow::UInt8Scalar>(255u));
    CHECK_RT(arrow::uint16(), std::make_shared<arrow::UInt16Scalar>(65535u));
    CHECK_RT(arrow::uint32(), std::make_shared<arrow::UInt32Scalar>(4'294'967'295u));
    CHECK_RT(arrow::uint64(), std::make_shared<arrow::UInt64Scalar>(18'446'744'073'709'551'615ull));
}

// ---------------------------------------------------------------------------
// Floating-point
// ---------------------------------------------------------------------------

TEST(ScalarTest, Float32AndFloat64Roundtrip) {
    CHECK_RT(arrow::float32(), std::make_shared<arrow::FloatScalar>(3.14f));
    CHECK_RT(arrow::float64(), std::make_shared<arrow::DoubleScalar>(2.718281828459045));
}

TEST(ScalarTest, HalfFloatRoundtrip) {
    // 0x3C00 = 1.0 in IEEE 754 half precision; 0xC000 = -2.0.
    CHECK_RT(arrow::float16(), std::make_shared<arrow::HalfFloatScalar>(0x3C00u));
    CHECK_RT(arrow::float16(), std::make_shared<arrow::HalfFloatScalar>(0xC000u));
}

// ---------------------------------------------------------------------------
// Date / time
// ---------------------------------------------------------------------------

TEST(ScalarTest, Date32AndDate64Roundtrip) {
    CHECK_RT(arrow::date32(), std::make_shared<arrow::Date32Scalar>(19000));
    CHECK_RT(arrow::date64(), std::make_shared<arrow::Date64Scalar>(1'640'000'000'000LL));
}

TEST(ScalarTest, TimestampWithoutTimezone) {
    auto type = arrow::timestamp(arrow::TimeUnit::SECOND);
    CHECK_RT(type, std::make_shared<arrow::TimestampScalar>(1'700'000'000LL, type));
}

TEST(ScalarTest, TimestampWithUtcMicrosecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
    CHECK_RT(type, std::make_shared<arrow::TimestampScalar>(1'700'000'000'000'000LL, type));
}

TEST(ScalarTest, TimestampWithNamedTimezoneNanosecond) {
    auto type = arrow::timestamp(arrow::TimeUnit::NANO, "America/New_York");
    CHECK_RT(type, std::make_shared<arrow::TimestampScalar>(-1'000'000'000LL, type));
}

TEST(ScalarTest, Time32Roundtrip) {
    auto type = arrow::time32(arrow::TimeUnit::MILLI);
    CHECK_RT(type, std::make_shared<arrow::Time32Scalar>(86'399'000, type));
}

TEST(ScalarTest, Time64Roundtrip) {
    auto type = arrow::time64(arrow::TimeUnit::NANO);
    CHECK_RT(type, std::make_shared<arrow::Time64Scalar>(86'399'999'999'999LL, type));
}

TEST(ScalarTest, DurationRoundtrip) {
    auto type = arrow::duration(arrow::TimeUnit::MILLI);
    CHECK_RT(type, std::make_shared<arrow::DurationScalar>(12'345LL, type));
}

// ---------------------------------------------------------------------------
// Intervals
// ---------------------------------------------------------------------------

TEST(ScalarTest, MonthIntervalRoundtrip) {
    CHECK_RT(arrow::month_interval(),
             std::make_shared<arrow::MonthIntervalScalar>(12));
}

TEST(ScalarTest, DayTimeIntervalRoundtrip) {
    arrow::DayTimeIntervalType::DayMilliseconds dm{5, 12'000};
    CHECK_RT(arrow::day_time_interval(),
             std::make_shared<arrow::DayTimeIntervalScalar>(dm));
}

TEST(ScalarTest, MonthDayNanoIntervalRoundtrip) {
    arrow::MonthDayNanoIntervalType::MonthDayNanos mdn{1, 2, 3'000'000'000LL};
    CHECK_RT(arrow::month_day_nano_interval(),
             std::make_shared<arrow::MonthDayNanoIntervalScalar>(mdn));
}

// ---------------------------------------------------------------------------
// Fixed-size binary
// ---------------------------------------------------------------------------

TEST(ScalarTest, FixedSizeBinaryRoundtrip) {
    auto type = arrow::fixed_size_binary(4);
    const uint8_t bytes[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto buf = std::make_shared<arrow::Buffer>(bytes, 4);
    CHECK_RT(type, std::make_shared<arrow::FixedSizeBinaryScalar>(buf, type));
}

// ---------------------------------------------------------------------------
// Decimal
// ---------------------------------------------------------------------------

TEST(ScalarTest, Decimal128Roundtrip) {
    auto type = arrow::decimal128(10, 3);
    CHECK_RT(type, std::make_shared<arrow::Decimal128Scalar>(
                       arrow::Decimal128("1234567"), type));
}

TEST(ScalarTest, Decimal256Roundtrip) {
    auto type = arrow::decimal256(40, 5);
    CHECK_RT(type, std::make_shared<arrow::Decimal256Scalar>(
                       arrow::Decimal256("12345678901234567890"), type));
}

// ---------------------------------------------------------------------------
// Variable-width binary / string
// ---------------------------------------------------------------------------

TEST(ScalarTest, StringAndLargeStringRoundtrip) {
    CHECK_RT(arrow::utf8(),       std::make_shared<arrow::StringScalar>("hello, world"));
    CHECK_RT(arrow::large_utf8(), std::make_shared<arrow::LargeStringScalar>("large string"));
}

TEST(ScalarTest, BinaryAndLargeBinaryRoundtrip) {
    // Use the 2-arg string constructor to preserve embedded null bytes.
    CHECK_RT(arrow::binary(),
             std::make_shared<arrow::BinaryScalar>(std::string("\x00\x01\x02\x03", 4)));
    CHECK_RT(arrow::large_binary(),
             std::make_shared<arrow::LargeBinaryScalar>(std::string("large\xff\xfe")));
}

TEST(ScalarTest, StringViewAndBinaryViewRoundtrip) {
    std::string sv_str = "string view content";
    auto sv_buf = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>(sv_str.data()),
        static_cast<int64_t>(sv_str.size()));
    CHECK_RT(arrow::utf8_view(), std::make_shared<arrow::StringViewScalar>(sv_buf));

    std::string bv_str("\x0A\x0B\x0C\x0D", 4);
    auto bv_buf = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>(bv_str.data()),
        static_cast<int64_t>(bv_str.size()));
    CHECK_RT(arrow::binary_view(), std::make_shared<arrow::BinaryViewScalar>(bv_buf));
}

TEST(ScalarTest, EmptyStringRoundtrip) {
    CHECK_RT(arrow::utf8(),   std::make_shared<arrow::StringScalar>(""));
    CHECK_RT(arrow::binary(), std::make_shared<arrow::BinaryScalar>(std::string()));
}
