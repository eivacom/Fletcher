// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-10 §3c: runtime codec edge/boundary coverage. These tests exercise the
// Arrow type families and numeric/varlen boundaries that the first-pass
// test_codec.cpp only touches once (or not at all): unions (sparse/dense with
// varlen children), intervals, time32/64, duration across units, decimals
// (incl. NEGATIVE), large/view string+binary, fixed-size-binary, integer
// min/max, IEEE-754 specials (NaN/±Inf/-0.0 compared BIT-EXACTLY, not via
// semantic Equals), embedded NULs / multi-byte UTF-8, wide null bitfields, and
// the concrete decode error paths (bad union type-code, truncated struct
// bitfield, fixed-size-binary width mismatch).
//
// Per locked decision #9, a GIR-10 coverage test may surface a real runtime
// codec bug (red-first) or stand as a regression guard. The runtime codec was
// hardened in Phase 1 (#98); these are predominantly regression guards plus the
// bit-exact float family (which a naive Equals-only test would mis-handle).
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fletcher/arrow_bridge/codec.hpp>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

// Encode -> decode one scalar under a single-field schema.
std::shared_ptr<arrow::Scalar> Roundtrip(const std::shared_ptr<arrow::DataType>& type,
                                         const std::shared_ptr<arrow::Scalar>& in) {
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    fletcher::Codec codec(schema);
    auto decoded = codec.DecodeRow(codec.EncodeRow({in}));
    if (decoded.size() != 1) {
        ADD_FAILURE() << "Expected decoded.size() == 1";
        return nullptr;
    }
    return decoded[0];
}

// Semantic round-trip (Arrow Equals) plus encode->decode->encode determinism:
// re-encoding the decoded scalar must reproduce the exact same bytes.
void ExpectSemanticRoundtripAndDeterminism(const std::shared_ptr<arrow::DataType>& type,
                                           const std::shared_ptr<arrow::Scalar>& in) {
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    fletcher::Codec codec(schema);
    auto bytes1 = codec.EncodeRow({in});
    auto decoded = codec.DecodeRow(bytes1);
    ASSERT_EQ(decoded.size(), 1u);
    ASSERT_TRUE(decoded[0]->Equals(*in)) << "decoded != original for " << type->ToString();
    auto bytes2 = codec.EncodeRow({decoded[0]});
    EXPECT_EQ(bytes1, bytes2) << "encode->decode->encode not deterministic for "
                              << type->ToString();
}

// Bit-exact float round-trip: the codec is a raw byte copy, so IEEE-754 payload
// bits (NaN mantissa, sign of zero, infinities) must survive verbatim. A
// semantic Equals would report NaN != NaN and -0.0 == +0.0, hiding corruption.
void ExpectFloatBitExact(float value) {
    auto in = std::make_shared<arrow::FloatScalar>(value);
    auto dec = Roundtrip(arrow::float32(), in);
    ASSERT_NE(dec, nullptr);
    float out = std::static_pointer_cast<arrow::FloatScalar>(dec)->value;
    uint32_t in_bits;
    uint32_t out_bits;
    std::memcpy(&in_bits, &value, sizeof(in_bits));
    std::memcpy(&out_bits, &out, sizeof(out_bits));
    EXPECT_EQ(in_bits, out_bits) << "float32 bit pattern changed";
}

void ExpectDoubleBitExact(double value) {
    auto in = std::make_shared<arrow::DoubleScalar>(value);
    auto dec = Roundtrip(arrow::float64(), in);
    ASSERT_NE(dec, nullptr);
    double out = std::static_pointer_cast<arrow::DoubleScalar>(dec)->value;
    uint64_t in_bits;
    uint64_t out_bits;
    std::memcpy(&in_bits, &value, sizeof(in_bits));
    std::memcpy(&out_bits, &out, sizeof(out_bits));
    EXPECT_EQ(in_bits, out_bits) << "float64 bit pattern changed";
}

}  // namespace

// ---------------------------------------------------------------------------
// Unions
// ---------------------------------------------------------------------------

TEST(CodecEdge, DenseUnionActiveScalarAndActiveVarlenChild) {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});

    // Active scalar child.
    ExpectSemanticRoundtripAndDeterminism(
        union_type, std::make_shared<arrow::DenseUnionScalar>(
                        std::make_shared<arrow::Int32Scalar>(-7), 0, union_type));

    // Active variable-length (string) child — with an embedded NUL for good measure.
    ExpectSemanticRoundtripAndDeterminism(
        union_type,
        std::make_shared<arrow::DenseUnionScalar>(
            std::make_shared<arrow::StringScalar>(std::string("a\0b", 3)), 1, union_type));
}

TEST(CodecEdge, SparseUnionMultipleActiveVariantsIncludingString) {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});

    ExpectSemanticRoundtripAndDeterminism(
        union_type,
        arrow::SparseUnionScalar::FromValue(std::make_shared<arrow::Int32Scalar>(123456),
                                            /*field_index=*/0, union_type));
    ExpectSemanticRoundtripAndDeterminism(
        union_type,
        arrow::SparseUnionScalar::FromValue(std::make_shared<arrow::StringScalar>("active-string"),
                                            /*field_index=*/1, union_type));
}

// ---------------------------------------------------------------------------
// Intervals
// ---------------------------------------------------------------------------

TEST(CodecEdge, IntervalVariantsRoundtrip) {
    ExpectSemanticRoundtripAndDeterminism(arrow::month_interval(),
                                          std::make_shared<arrow::MonthIntervalScalar>(-13));

    arrow::DayTimeIntervalType::DayMilliseconds dt{};
    dt.days = -5;
    dt.milliseconds = 86'399'000;
    ExpectSemanticRoundtripAndDeterminism(arrow::day_time_interval(),
                                          std::make_shared<arrow::DayTimeIntervalScalar>(dt));

    arrow::MonthDayNanoIntervalType::MonthDayNanos mdn{};
    mdn.months = 14;
    mdn.days = -2;
    mdn.nanoseconds = -1'234'567'890LL;
    ExpectSemanticRoundtripAndDeterminism(arrow::month_day_nano_interval(),
                                          std::make_shared<arrow::MonthDayNanoIntervalScalar>(mdn));
}

// ---------------------------------------------------------------------------
// Time32 / Time64 across units
// ---------------------------------------------------------------------------

TEST(CodecEdge, Time32AcrossUnits) {
    auto t_sec = arrow::time32(arrow::TimeUnit::SECOND);
    ExpectSemanticRoundtripAndDeterminism(t_sec,
                                          std::make_shared<arrow::Time32Scalar>(86'399, t_sec));
    auto t_milli = arrow::time32(arrow::TimeUnit::MILLI);
    ExpectSemanticRoundtripAndDeterminism(
        t_milli, std::make_shared<arrow::Time32Scalar>(86'399'999, t_milli));
}

TEST(CodecEdge, Time64AcrossUnits) {
    auto t_micro = arrow::time64(arrow::TimeUnit::MICRO);
    ExpectSemanticRoundtripAndDeterminism(
        t_micro, std::make_shared<arrow::Time64Scalar>(86'399'999'999LL, t_micro));
    auto t_nano = arrow::time64(arrow::TimeUnit::NANO);
    ExpectSemanticRoundtripAndDeterminism(
        t_nano, std::make_shared<arrow::Time64Scalar>(86'399'999'999'999LL, t_nano));
}

// ---------------------------------------------------------------------------
// Duration across units
// ---------------------------------------------------------------------------

TEST(CodecEdge, DurationAcrossUnits) {
    for (auto unit : {arrow::TimeUnit::SECOND, arrow::TimeUnit::MILLI, arrow::TimeUnit::MICRO,
                      arrow::TimeUnit::NANO}) {
        auto type = arrow::duration(unit);
        ExpectSemanticRoundtripAndDeterminism(
            type, std::make_shared<arrow::DurationScalar>(-9'223'372'036'854'775'807LL, type));
    }
}

// ---------------------------------------------------------------------------
// Decimals — positive, zero, NEGATIVE, and precision-scale extremes
// ---------------------------------------------------------------------------

TEST(CodecEdge, Decimal128PositiveZeroNegativeAndMax) {
    auto type = arrow::decimal128(38, 10);
    ExpectSemanticRoundtripAndDeterminism(
        type, std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(12345), type));
    ExpectSemanticRoundtripAndDeterminism(
        type, std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(0), type));
    // NEGATIVE — a sign-extension / two's-complement byte-order regression would
    // corrupt this while leaving the positive case intact.
    ExpectSemanticRoundtripAndDeterminism(
        type, std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(-987654321LL), type));
    ExpectSemanticRoundtripAndDeterminism(
        type, std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128::GetMaxValue(38), type));
}

TEST(CodecEdge, Decimal256NegativeRoundtrips) {
    auto type = arrow::decimal256(50, 6);
    ExpectSemanticRoundtripAndDeterminism(
        type, std::make_shared<arrow::Decimal256Scalar>(arrow::Decimal256(-123456789LL), type));
}

// ---------------------------------------------------------------------------
// Large string / large binary, string/binary view
// ---------------------------------------------------------------------------

TEST(CodecEdge, LargeStringAndLargeBinaryRoundtrip) {
    auto ls = std::make_shared<arrow::LargeStringScalar>(std::string("large-utf8-\xC3\xA9", 12));
    ExpectSemanticRoundtripAndDeterminism(arrow::large_utf8(), ls);

    auto buf = arrow::Buffer::FromString(std::string("\x00\x01\x02\xFF", 4));
    ExpectSemanticRoundtripAndDeterminism(arrow::large_binary(),
                                          std::make_shared<arrow::LargeBinaryScalar>(buf));
}

TEST(CodecEdge, StringViewAndBinaryViewRoundtrip) {
    auto sv = std::make_shared<arrow::StringViewScalar>(
        arrow::Buffer::FromString("a string-view value long enough to spill inline"));
    ExpectSemanticRoundtripAndDeterminism(arrow::utf8_view(), sv);

    auto bv = std::make_shared<arrow::BinaryViewScalar>(
        arrow::Buffer::FromString(std::string("\xDE\xAD\x00\xBE\xEF", 5)));
    ExpectSemanticRoundtripAndDeterminism(arrow::binary_view(), bv);
}

// ---------------------------------------------------------------------------
// Fixed-size binary
// ---------------------------------------------------------------------------

TEST(CodecEdge, FixedSizeBinaryRoundtrip) {
    auto type = arrow::fixed_size_binary(4);
    auto buf = arrow::Buffer::FromString(std::string("\x00\xFF\x10\x7F", 4));
    ExpectSemanticRoundtripAndDeterminism(
        type, std::make_shared<arrow::FixedSizeBinaryScalar>(buf, type));
}

// ---------------------------------------------------------------------------
// Integer boundaries: INT*_MIN, INT*_MAX, UINT*_MAX
// ---------------------------------------------------------------------------

TEST(CodecEdge, IntegerBoundaries) {
    using Lim8 = std::numeric_limits<int8_t>;
    using Lim16 = std::numeric_limits<int16_t>;
    using Lim32 = std::numeric_limits<int32_t>;
    using Lim64 = std::numeric_limits<int64_t>;

    ExpectSemanticRoundtripAndDeterminism(arrow::int8(),
                                          std::make_shared<arrow::Int8Scalar>(Lim8::min()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int8(),
                                          std::make_shared<arrow::Int8Scalar>(Lim8::max()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int16(),
                                          std::make_shared<arrow::Int16Scalar>(Lim16::min()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int16(),
                                          std::make_shared<arrow::Int16Scalar>(Lim16::max()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int32(),
                                          std::make_shared<arrow::Int32Scalar>(Lim32::min()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int32(),
                                          std::make_shared<arrow::Int32Scalar>(Lim32::max()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int64(),
                                          std::make_shared<arrow::Int64Scalar>(Lim64::min()));
    ExpectSemanticRoundtripAndDeterminism(arrow::int64(),
                                          std::make_shared<arrow::Int64Scalar>(Lim64::max()));

    ExpectSemanticRoundtripAndDeterminism(
        arrow::uint8(), std::make_shared<arrow::UInt8Scalar>(std::numeric_limits<uint8_t>::max()));
    ExpectSemanticRoundtripAndDeterminism(
        arrow::uint16(),
        std::make_shared<arrow::UInt16Scalar>(std::numeric_limits<uint16_t>::max()));
    ExpectSemanticRoundtripAndDeterminism(
        arrow::uint32(),
        std::make_shared<arrow::UInt32Scalar>(std::numeric_limits<uint32_t>::max()));
    ExpectSemanticRoundtripAndDeterminism(
        arrow::uint64(),
        std::make_shared<arrow::UInt64Scalar>(std::numeric_limits<uint64_t>::max()));
}

// ---------------------------------------------------------------------------
// IEEE-754 specials — bit-exact (NaN payload, ±Inf, -0.0)
// ---------------------------------------------------------------------------

TEST(CodecEdge, Float32SpecialsBitExact) {
    ExpectFloatBitExact(std::numeric_limits<float>::quiet_NaN());
    ExpectFloatBitExact(std::numeric_limits<float>::infinity());
    ExpectFloatBitExact(-std::numeric_limits<float>::infinity());
    ExpectFloatBitExact(-0.0f);
    // NaN with an explicit non-canonical payload.
    uint32_t payload_bits = 0x7FC0BEEFu;
    float payload_nan;
    std::memcpy(&payload_nan, &payload_bits, sizeof(payload_nan));
    ExpectFloatBitExact(payload_nan);
}

TEST(CodecEdge, Float64SpecialsBitExact) {
    ExpectDoubleBitExact(std::numeric_limits<double>::quiet_NaN());
    ExpectDoubleBitExact(std::numeric_limits<double>::infinity());
    ExpectDoubleBitExact(-std::numeric_limits<double>::infinity());
    ExpectDoubleBitExact(-0.0);
    uint64_t payload_bits = 0x7FF8000000C0FFEEull;
    double payload_nan;
    std::memcpy(&payload_nan, &payload_bits, sizeof(payload_nan));
    ExpectDoubleBitExact(payload_nan);
}

TEST(CodecEdge, HalfFloatBitExact) {
    // HalfFloat is transported as its raw uint16 bit pattern; +Inf = 0x7C00,
    // a NaN = 0x7E00, -0.0 = 0x8000. Round-trip must preserve every bit.
    for (uint16_t bits : {uint16_t{0x7C00}, uint16_t{0xFC00}, uint16_t{0x7E00}, uint16_t{0x8000}}) {
        auto dec = Roundtrip(arrow::float16(), std::make_shared<arrow::HalfFloatScalar>(bits));
        ASSERT_NE(dec, nullptr);
        EXPECT_EQ(std::static_pointer_cast<arrow::HalfFloatScalar>(dec)->value, bits);
    }
}

// ---------------------------------------------------------------------------
// Binary-safe varlen: empty, embedded NULs, multi-byte UTF-8
// ---------------------------------------------------------------------------

TEST(CodecEdge, StringEmbeddedNulAndMultiByteUtf8) {
    ExpectSemanticRoundtripAndDeterminism(arrow::utf8(), std::make_shared<arrow::StringScalar>(""));
    // Embedded NUL: a naive strlen-based length would truncate at the NUL.
    ExpectSemanticRoundtripAndDeterminism(
        arrow::utf8(), std::make_shared<arrow::StringScalar>(std::string("before\0after", 12)));
    // Multi-byte UTF-8: "héllo→€" (mixed 1/2/3-byte code points).
    ExpectSemanticRoundtripAndDeterminism(
        arrow::utf8(),
        std::make_shared<arrow::StringScalar>("h\xC3\xA9llo\xE2\x86\x92\xE2\x82\xAC"));
}

TEST(CodecEdge, BinaryEmptyAndEmbeddedZeroBytes) {
    ExpectSemanticRoundtripAndDeterminism(
        arrow::binary(), std::make_shared<arrow::BinaryScalar>(arrow::Buffer::FromString("")));
    ExpectSemanticRoundtripAndDeterminism(
        arrow::binary(), std::make_shared<arrow::BinaryScalar>(arrow::Buffer::FromString(
                             std::string("\x00\x00\x01\x00\xFF\x00", 6))));
}

// ---------------------------------------------------------------------------
// Empty list / map, all-null list, high-index nulls
// ---------------------------------------------------------------------------

TEST(CodecEdge, EmptyListAndEmptyMapRoundtrip) {
    {
        auto list_type = arrow::list(arrow::int32());
        auto empty = arrow::Int32Builder().Finish().ValueOrDie();
        ExpectSemanticRoundtripAndDeterminism(list_type,
                                              std::make_shared<arrow::ListScalar>(empty));
    }
    {
        auto map_type = arrow::map(arrow::utf8(), arrow::int32());
        const auto& mt = static_cast<const arrow::MapType&>(*map_type);
        auto keys = arrow::StringBuilder().Finish().ValueOrDie();
        auto vals = arrow::Int32Builder().Finish().ValueOrDie();
        auto entries =
            arrow::StructArray::Make({keys, vals}, mt.value_type()->fields()).ValueOrDie();
        ExpectSemanticRoundtripAndDeterminism(
            map_type, std::make_shared<arrow::MapScalar>(entries, map_type));
    }
}

TEST(CodecEdge, WideNullBitfieldHighIndexNullsInListAndRow) {
    // A list of 17 elements with only the highest index non-null exercises the
    // ceil(count/8)-byte element bitfield and high-bit indexing (bit 16 in byte 2).
    auto list_type = arrow::list(arrow::int32());
    arrow::Int32Builder b;
    for (int i = 0; i < 16; ++i) ASSERT_TRUE(b.AppendNull().ok());
    ASSERT_TRUE(b.Append(0x0BADF00D).ok());
    auto arr = b.Finish().ValueOrDie();
    ExpectSemanticRoundtripAndDeterminism(list_type, std::make_shared<arrow::ListScalar>(arr));

    // A row of 17 fields (3-byte row bitfield) with a null only at the high index.
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fletcher::ArrowRow row;
    for (int i = 0; i < 17; ++i) {
        fields.push_back(arrow::field("f" + std::to_string(i), arrow::int32(), true));
        row.push_back(i == 16 ? arrow::MakeNullScalar(arrow::int32())
                              : std::static_pointer_cast<arrow::Scalar>(
                                    std::make_shared<arrow::Int32Scalar>(i)));
    }
    fletcher::Codec codec(arrow::schema(fields));
    auto decoded = codec.DecodeRow(codec.EncodeRow(row));
    ASSERT_EQ(decoded.size(), 17u);
    EXPECT_FALSE(decoded[16]->is_valid);
    for (int i = 0; i < 16; ++i) EXPECT_TRUE(decoded[i]->Equals(*row[i]));
}

// ---------------------------------------------------------------------------
// Concrete decode error paths (distinct from GIR-11 fuzz)
// ---------------------------------------------------------------------------

TEST(CodecEdge, DecodeRejectsBadDenseUnionTypeCode) {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type)});
    fletcher::Codec codec(schema);
    // [row bitfield 0x00 (present)] [type_code = 0x09 — no such child].
    const std::vector<uint8_t> buf = {0x00, 0x09};
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
}

TEST(CodecEdge, DecodeRejectsBadSparseUnionTypeCode) {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type)});
    fletcher::Codec codec(schema);
    const std::vector<uint8_t> buf = {0x00, 0x7F};
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
}

TEST(CodecEdge, DecodeRejectsTruncatedStructBitfield) {
    // A struct field's leading null-bitfield byte is missing entirely.
    auto stype =
        arrow::struct_({arrow::field("x", arrow::int32()), arrow::field("y", arrow::int32())});
    auto schema = arrow::schema({arrow::field("s", stype)});
    fletcher::Codec codec(schema);
    // [row bitfield 0x00 (struct present)] and nothing else — the struct's own
    // bitfield byte is truncated.
    const std::vector<uint8_t> buf = {0x00};
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
}

TEST(CodecEdge, DecodeRejectsFixedSizeBinaryWidthTruncation) {
    auto type = arrow::fixed_size_binary(8);
    auto schema = arrow::schema({arrow::field("v", type)});
    fletcher::Codec codec(schema);
    // Present field but only 3 of the 8 required payload bytes are supplied.
    const std::vector<uint8_t> buf = {0x00, 0x01, 0x02, 0x03};
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
}
