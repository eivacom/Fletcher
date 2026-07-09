// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fletcher/arrow_bridge/arrow_row_view.hpp>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/positional_io.hpp>
#include <memory>
#include <stdexcept>
#include <vector>

// Internal headers (arrow-bridge/src) — exercised directly by the HARD-1
// scalar-level forcing tests below.
#include "row_reader.hpp"
#include "scalar_codec.hpp"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<arrow::Scalar> Roundtrip(const std::shared_ptr<arrow::DataType>& type,
                                         const std::shared_ptr<arrow::Scalar>& in) {
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    fletcher::Codec codec(schema);
    auto row = codec.EncodeRow({in});
    auto decoded = codec.DecodeRow(row);
    if (decoded.size() != 1) {
        ADD_FAILURE() << "Expected decoded.size() == 1";
        return nullptr;
    }
    return decoded[0];
}

#define CHECK_POS_RT(type_expr, scalar_expr)       \
    do {                                           \
        auto _orig = (scalar_expr);                \
        auto _dec = Roundtrip((type_expr), _orig); \
        ASSERT_NE(_dec, nullptr);                  \
        EXPECT_TRUE(_dec->Equals(*_orig));         \
    } while (false)

}  // namespace

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

TEST(CodecTest, BooleanRoundtrip) {
    CHECK_POS_RT(arrow::boolean(), std::make_shared<arrow::BooleanScalar>(true));
    CHECK_POS_RT(arrow::boolean(), std::make_shared<arrow::BooleanScalar>(false));
}

TEST(CodecTest, IntegerRoundtrip) {
    CHECK_POS_RT(arrow::int8(), std::make_shared<arrow::Int8Scalar>(-42));
    CHECK_POS_RT(arrow::int16(), std::make_shared<arrow::Int16Scalar>(1234));
    CHECK_POS_RT(arrow::int32(), std::make_shared<arrow::Int32Scalar>(-1'000'000));
    CHECK_POS_RT(arrow::int64(), std::make_shared<arrow::Int64Scalar>(9'223'372'036'854'775'807LL));
    CHECK_POS_RT(arrow::uint8(), std::make_shared<arrow::UInt8Scalar>(255u));
    CHECK_POS_RT(arrow::uint16(), std::make_shared<arrow::UInt16Scalar>(65535u));
    CHECK_POS_RT(arrow::uint32(), std::make_shared<arrow::UInt32Scalar>(4'294'967'295u));
    CHECK_POS_RT(arrow::uint64(),
                 std::make_shared<arrow::UInt64Scalar>(18'446'744'073'709'551'615ull));
}

TEST(CodecTest, FloatRoundtrip) {
    CHECK_POS_RT(arrow::float32(), std::make_shared<arrow::FloatScalar>(3.14f));
    CHECK_POS_RT(arrow::float64(), std::make_shared<arrow::DoubleScalar>(2.718281828));
    CHECK_POS_RT(arrow::float16(), std::make_shared<arrow::HalfFloatScalar>(0x3C00u));
}

TEST(CodecTest, StringAndBinaryRoundtrip) {
    auto str = std::make_shared<arrow::StringScalar>("hello positional");
    CHECK_POS_RT(arrow::utf8(), str);

    auto bin_data =
        std::make_shared<arrow::Buffer>(reinterpret_cast<const uint8_t*>("\xDE\xAD\xBE\xEF"), 4);
    auto bin = std::make_shared<arrow::BinaryScalar>(bin_data);
    CHECK_POS_RT(arrow::binary(), bin);
}

TEST(CodecTest, TemporalTypesRoundtrip) {
    CHECK_POS_RT(arrow::date32(), std::make_shared<arrow::Date32Scalar>(19000));
    CHECK_POS_RT(arrow::date64(), std::make_shared<arrow::Date64Scalar>(1'640'995'200'000LL));
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    CHECK_POS_RT(ts_type, std::make_shared<arrow::TimestampScalar>(1'000'000'000LL, ts_type));
}

TEST(CodecTest, DecimalRoundtrip) {
    auto dec128_type = arrow::decimal128(10, 2);
    CHECK_POS_RT(dec128_type,
                 std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(12345), dec128_type));
}

// ---------------------------------------------------------------------------
// HARD-1: DecodeScalarFromReader defects (#52 owned copy, #58 dead tail throw)
// ---------------------------------------------------------------------------

// #52 forcing test (red-first). A decoded FixedSizeBinary scalar must OWN its
// bytes. Decode from a test-owned mutable buffer, then OVERWRITE that exact
// source region with a different pattern and confirm the scalar still reads the
// ORIGINAL bytes. Overwriting (not freeing) is deliberate: freeing then reading
// a dangling alias is UB that commonly returns stale-correct bytes, which would
// false-green the pre-fix aliasing bug (locked decision #9).
TEST(CodecTest, FixedSizeBinaryOwnsBytesAfterSourceFreed) {
    constexpr int kWidth = 4;
    auto type = arrow::fixed_size_binary(kWidth);

    std::vector<uint8_t> src = {0x01, 0x02, 0x03, 0x04};
    const std::vector<uint8_t> original = src;

    fletcher::detail::Reader r{src.data(), src.size()};
    auto scalar = fletcher::detail::DecodeScalarFromReader(r, type);
    ASSERT_NE(scalar, nullptr);
    ASSERT_EQ(scalar->type->id(), arrow::Type::FIXED_SIZE_BINARY);
    const auto& fsb = static_cast<const arrow::FixedSizeBinaryScalar&>(*scalar);
    ASSERT_NE(fsb.value, nullptr);
    ASSERT_EQ(fsb.value->size(), static_cast<int64_t>(kWidth));

    // Overwrite the exact backing bytes with a different pattern post-decode.
    std::fill(src.begin(), src.end(), static_cast<uint8_t>(0xAA));

    // The scalar must still expose the original bytes (owned copy), not 0xAA.
    EXPECT_EQ(0, std::memcmp(fsb.value->data(), original.data(), kWidth));
}

// #52 + #58: round-trip every string/binary/fixed-size-binary variant. This
// covers the FixedSizeBinary owned-copy path and all six string/binary inner
// switch return arms, and confirms H-INV-1 (bytes unchanged) for these families.
TEST(CodecTest, StringBinaryFixedSizeVariantRoundtrip) {
    CHECK_POS_RT(arrow::utf8(),
                 std::make_shared<arrow::StringScalar>(arrow::Buffer::FromString("utf8-value")));
    CHECK_POS_RT(arrow::large_utf8(), std::make_shared<arrow::LargeStringScalar>(
                                          arrow::Buffer::FromString("large-utf8-value")));
    CHECK_POS_RT(arrow::binary(), std::make_shared<arrow::BinaryScalar>(arrow::Buffer::FromString(
                                      std::string("\x00\x01\x02\xFF", 4))));
    CHECK_POS_RT(arrow::large_binary(), std::make_shared<arrow::LargeBinaryScalar>(
                                            arrow::Buffer::FromString("large-binary-value")));
    CHECK_POS_RT(arrow::utf8_view(), std::make_shared<arrow::StringViewScalar>(
                                         arrow::Buffer::FromString("string-view-value")));
    CHECK_POS_RT(arrow::binary_view(), std::make_shared<arrow::BinaryViewScalar>(
                                           arrow::Buffer::FromString("binary-view-value")));
    {
        constexpr int kWidth = 6;
        auto fsb_type = arrow::fixed_size_binary(kWidth);
        CHECK_POS_RT(fsb_type, std::make_shared<arrow::FixedSizeBinaryScalar>(
                                   arrow::Buffer::FromString("abcdef"), fsb_type));
    }
}

// #58 green regression guard. arrow::list(int32) has type id LIST, which is not
// handled by DecodeScalarFromReader, so it reaches the reachable outer `default`
// throw. This confirms that deleting the duplicate unreachable tail throw does
// not let the unsupported-type path fall off the function without throwing.
TEST(CodecTest, DecodeScalarUnsupportedTypeThrowsInvalidArgument) {
    std::vector<uint8_t> src(32, 0x00);
    fletcher::detail::Reader r{src.data(), src.size()};
    EXPECT_THROW(fletcher::detail::DecodeScalarFromReader(r, arrow::list(arrow::int32())),
                 std::invalid_argument);
}

// ---------------------------------------------------------------------------
// HARD-2: Checked Arrow Result<T> access (#53, runtime half)
//
// Runtime paths that previously called .ValueOrDie() on a failed Arrow
// Result<T> aborted the process. They must now throw std::invalid_argument
// (H-INV-2). These forcing tests drive a failed Result<T> through a public
// entry point and assert the throw. Red-first (locked decision #9): before the
// fix each path reaches .ValueOrDie() on a failed Result, which calls abort()
// and CRASHES the test binary (valid red — process termination, not a catchable
// exception). Post-fix the same failed Result becomes a catchable
// std::invalid_argument, so these use EXPECT_THROW (not a death test).
// ---------------------------------------------------------------------------

namespace {

// Satisfies ArrowRowViewList<ViewT>'s contract (arrow_row_view.hpp:59-60):
// constructible from std::shared_ptr<arrow::Scalar>.
class DummyStructView {
   public:
    explicit DummyStructView(std::shared_ptr<arrow::Scalar> scalar) : scalar_(std::move(scalar)) {}

   private:
    std::shared_ptr<arrow::Scalar> scalar_;
};

}  // namespace

// Exercises the arrow_row_view.hpp path (ArrowRowViewList::operator[], line 74).
// A struct array with one element (index 0 valid) is accessed out of range
// (index 1), so array_->GetScalar(1) returns a failed Result.
TEST(CodecTest, BadResultThrowsInsteadOfAbort) {
    auto stype = arrow::struct_({arrow::field("x", arrow::int32())});

    std::shared_ptr<arrow::Array> array;
    arrow::StructBuilder builder(stype, arrow::default_memory_pool(),
                                 {std::make_shared<arrow::Int32Builder>()});
    ASSERT_TRUE(builder.Append().ok());
    auto* x_builder = static_cast<arrow::Int32Builder*>(builder.field_builder(0));
    ASSERT_TRUE(x_builder->Append(7).ok());
    ASSERT_TRUE(builder.Finish(&array).ok());

    fletcher::ArrowRowViewList<DummyStructView> views(array);

    // Pre-fix: views[1] reaches array_->GetScalar(1).ValueOrDie() -> abort().
    // Post-fix: the failed Result becomes a catchable std::invalid_argument.
    EXPECT_THROW(static_cast<void>(views[1]), std::invalid_argument);
}

// Exercises a codec.cpp path (EncodePositionalValue MAP case, key GetScalar at
// line 139) through the public Codec::EncodeRow. A deliberately-malformed map
// entries StructArray declares length 2 but carries a length-1 key child;
// StructArray::field() clamps the child to its own buffer (Slice does not
// over-read), so the encode loop's key_arr->GetScalar(1) on that length-1 array
// returns a failed Result. This is a genuine codec.cpp call site reached via a
// public API entry point with untrusted/malformed input.
TEST(CodecTest, CodecMapEncodeBadResultThrowsInsteadOfAbort) {
    auto map_type = arrow::map(arrow::utf8(), arrow::int32());
    const auto& mt = static_cast<const arrow::MapType&>(*map_type);
    auto entries_type = mt.value_type();  // struct<key, value>

    arrow::StringBuilder kb;
    ASSERT_TRUE(kb.Append("a").ok());
    std::shared_ptr<arrow::Array> keys;
    ASSERT_TRUE(kb.Finish(&keys).ok());  // length 1

    arrow::Int32Builder vb;
    ASSERT_TRUE(vb.Append(1).ok());
    std::shared_ptr<arrow::Array> vals;
    ASSERT_TRUE(vb.Finish(&vals).ok());  // length 1

    // Malformed: declared length 2, but the children are length 1.
    auto entries = std::make_shared<arrow::StructArray>(entries_type, /*length=*/2,
                                                        arrow::ArrayVector{keys, vals});
    std::shared_ptr<arrow::Scalar> map_scalar =
        std::make_shared<arrow::MapScalar>(entries, map_type);

    auto schema = arrow::schema({arrow::field("m", map_type)});
    fletcher::Codec codec(schema);

    // Pre-fix: the key loop reaches key_arr->GetScalar(1).ValueOrDie() -> abort().
    // Post-fix: the failed Result becomes a catchable std::invalid_argument.
    EXPECT_THROW(static_cast<void>(codec.EncodeRow({map_scalar})), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Nulls
// ---------------------------------------------------------------------------

TEST(CodecTest, NullFields) {
    auto schema = arrow::schema({
        arrow::field("a", arrow::int32(), true),
        arrow::field("b", arrow::utf8(), true),
        arrow::field("c", arrow::float64(), true),
    });
    fletcher::Codec codec(schema);

    fletcher::ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(42),
        arrow::MakeNullScalar(arrow::utf8()),
        std::make_shared<arrow::DoubleScalar>(3.14),
    };

    auto encoded = codec.EncodeRow(row);
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_TRUE(decoded[0]->Equals(*row[0]));
    EXPECT_FALSE(decoded[1]->is_valid);
    EXPECT_TRUE(decoded[2]->Equals(*row[2]));
}

TEST(CodecTest, AllNullFields) {
    auto schema = arrow::schema({
        arrow::field("a", arrow::int32(), true),
        arrow::field("b", arrow::utf8(), true),
    });
    fletcher::Codec codec(schema);

    fletcher::ArrowRow row = {
        arrow::MakeNullScalar(arrow::int32()),
        arrow::MakeNullScalar(arrow::utf8()),
    };

    auto encoded = codec.EncodeRow(row);
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_FALSE(decoded[0]->is_valid);
    EXPECT_FALSE(decoded[1]->is_valid);

    // All-null row should just be the bitfield: 1 byte with bits 0 and 1 set.
    ASSERT_EQ(encoded.size(), 1u);
    EXPECT_EQ(encoded[0], 0x03);
}

// ---------------------------------------------------------------------------
// Multi-field row
// ---------------------------------------------------------------------------

TEST(CodecTest, MultiFieldRowRoundtrip) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32()),
        arrow::field("name", arrow::utf8()),
        arrow::field("active", arrow::boolean()),
        arrow::field("score", arrow::float64()),
    });
    fletcher::Codec codec(schema);

    fletcher::ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(100),
        std::make_shared<arrow::StringScalar>("alice"),
        std::make_shared<arrow::BooleanScalar>(true),
        std::make_shared<arrow::DoubleScalar>(99.5),
    };

    auto decoded = codec.DecodeRow(codec.EncodeRow(row));
    ASSERT_EQ(decoded.size(), 4u);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(decoded[i]->Equals(*row[i]));
}

// ---------------------------------------------------------------------------
// Struct
// ---------------------------------------------------------------------------

TEST(CodecTest, StructRoundtrip) {
    auto stype = arrow::struct_({
        arrow::field("x", arrow::int32()),
        arrow::field("y", arrow::utf8()),
    });
    auto schema = arrow::schema({arrow::field("s", stype)});
    fletcher::Codec codec(schema);

    auto struct_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{
            std::make_shared<arrow::Int32Scalar>(7),
            std::make_shared<arrow::StringScalar>("hi"),
        },
        stype);

    auto decoded = codec.DecodeRow(codec.EncodeRow({struct_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*struct_scalar));
}

TEST(CodecTest, StructWithNullChild) {
    auto stype = arrow::struct_({
        arrow::field("x", arrow::int32(), true),
        arrow::field("y", arrow::utf8(), true),
    });
    auto schema = arrow::schema({arrow::field("s", stype)});
    fletcher::Codec codec(schema);

    auto struct_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{
            arrow::MakeNullScalar(arrow::int32()),
            std::make_shared<arrow::StringScalar>("hello"),
        },
        stype);

    auto decoded = codec.DecodeRow(codec.EncodeRow({struct_scalar}));
    ASSERT_EQ(decoded.size(), 1u);

    auto& ds = static_cast<const arrow::StructScalar&>(*decoded[0]);
    EXPECT_FALSE(ds.value[0]->is_valid);
    EXPECT_TRUE(ds.value[1]->Equals(*std::make_shared<arrow::StringScalar>("hello")));
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

TEST(CodecTest, ListRoundtrip) {
    auto list_type = arrow::list(arrow::int32());
    auto schema = arrow::schema({arrow::field("nums", list_type)});
    fletcher::Codec codec(schema);

    auto builder = arrow::Int32Builder();
    ASSERT_TRUE(builder.AppendValues({10, 20, 30}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

TEST(CodecTest, ListWithNulls) {
    auto list_type = arrow::list(arrow::int32());
    auto schema = arrow::schema({arrow::field("nums", list_type)});
    fletcher::Codec codec(schema);

    auto builder = arrow::Int32Builder();
    ASSERT_TRUE(builder.Append(1).ok());
    ASSERT_TRUE(builder.AppendNull().ok());
    ASSERT_TRUE(builder.Append(3).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

TEST(CodecTest, ListOfAllNullElementsRoundtrips) {
    // Regression (PR #98 review): a list whose elements are all null carries no
    // payload bytes, so its element count legitimately exceeds the remaining
    // byte count. The decode bound is on the null-bitfield size (ceil(count/8)),
    // not the raw count — otherwise this valid buffer would be rejected.
    auto list_type = arrow::list(arrow::int32());
    auto schema = arrow::schema({arrow::field("nums", list_type)});
    fletcher::Codec codec(schema);

    auto builder = arrow::Int32Builder();
    for (int i = 0; i < 20; ++i) ASSERT_TRUE(builder.AppendNull().ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

// ---------------------------------------------------------------------------
// Fixed-size list
// ---------------------------------------------------------------------------

TEST(CodecTest, FixedSizeListRoundtrip) {
    auto fsl_type = arrow::fixed_size_list(arrow::float32(), 3);
    auto schema = arrow::schema({arrow::field("vec", fsl_type)});
    fletcher::Codec codec(schema);

    auto builder = arrow::FloatBuilder();
    ASSERT_TRUE(builder.AppendValues({1.0f, 2.0f, 3.0f}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto fsl_scalar = std::make_shared<arrow::FixedSizeListScalar>(arr, fsl_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({fsl_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*fsl_scalar));
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

TEST(CodecTest, MapRoundtrip) {
    auto map_type = arrow::map(arrow::utf8(), arrow::int32());
    auto schema = arrow::schema({arrow::field("m", map_type)});
    fletcher::Codec codec(schema);

    auto key_builder_ptr = arrow::StringBuilder();
    auto val_builder_ptr = arrow::Int32Builder();
    ASSERT_TRUE(key_builder_ptr.Append("a").ok());
    ASSERT_TRUE(val_builder_ptr.Append(1).ok());
    ASSERT_TRUE(key_builder_ptr.Append("b").ok());
    ASSERT_TRUE(val_builder_ptr.Append(2).ok());
    auto keys = key_builder_ptr.Finish().ValueOrDie();
    auto vals = val_builder_ptr.Finish().ValueOrDie();

    auto entries_type = arrow::struct_(
        {arrow::field("key", arrow::utf8(), false), arrow::field("value", arrow::int32())});
    auto entries =
        std::make_shared<arrow::StructArray>(entries_type, 2, arrow::ArrayVector{keys, vals});
    auto map_scalar = std::make_shared<arrow::MapScalar>(entries);

    auto decoded = codec.DecodeRow(codec.EncodeRow({map_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*map_scalar));
}

// ---------------------------------------------------------------------------
// Union
// ---------------------------------------------------------------------------

TEST(CodecTest, DenseUnionRoundtrip) {
    auto union_type = arrow::dense_union(
        {
            arrow::field("i", arrow::int32()),
            arrow::field("s", arrow::utf8()),
        },
        {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type)});
    fletcher::Codec codec(schema);

    // Encode an int variant.
    auto int_val = std::make_shared<arrow::Int32Scalar>(42);
    auto union_scalar = std::make_shared<arrow::DenseUnionScalar>(int_val, 0, union_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({union_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*union_scalar));
}

// ---------------------------------------------------------------------------
// Wire size
// ---------------------------------------------------------------------------

TEST(CodecTest, CompactWireSize) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32()),
        arrow::field("x", arrow::float64()),
        arrow::field("y", arrow::float64()),
        arrow::field("z", arrow::float64()),
        arrow::field("name", arrow::utf8()),
        arrow::field("active", arrow::boolean()),
    });

    fletcher::Codec codec(schema);
    fletcher::ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::DoubleScalar>(1.0),
        std::make_shared<arrow::DoubleScalar>(2.0),
        std::make_shared<arrow::DoubleScalar>(3.0),
        std::make_shared<arrow::StringScalar>("sensor"),
        std::make_shared<arrow::BooleanScalar>(true),
    };

    auto encoded = codec.EncodeRow(row);

    // Positional: 1 byte bitfield + 4 + 8 + 8 + 8 + (4+6) + 1 = 40 bytes
    SCOPED_TRACE("Encoded size: " + std::to_string(encoded.size()));
    EXPECT_LT(encoded.size(), 50u);
}

// ---------------------------------------------------------------------------
// 9+ fields (multi-byte bitfield)
// ---------------------------------------------------------------------------

TEST(CodecTest, NinePlusFieldsUseMultiByteNullBitfield) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fletcher::ArrowRow row;
    for (int i = 0; i < 12; ++i) {
        fields.push_back(arrow::field("f" + std::to_string(i), arrow::int32(), true));
        if (i % 3 == 0) {
            row.push_back(arrow::MakeNullScalar(arrow::int32()));
        } else {
            row.push_back(std::make_shared<arrow::Int32Scalar>(i * 10));
        }
    }
    auto schema = arrow::schema(fields);
    fletcher::Codec codec(schema);

    auto decoded = codec.DecodeRow(codec.EncodeRow(row));
    ASSERT_EQ(decoded.size(), 12u);
    for (int i = 0; i < 12; ++i) {
        if (i % 3 == 0) {
            EXPECT_FALSE(decoded[i]->is_valid);
        } else {
            EXPECT_TRUE(decoded[i]->Equals(*row[i]));
        }
    }
}

// ---------------------------------------------------------------------------
// Dictionary fields — transferred as the value type, one value per row.
// ---------------------------------------------------------------------------

TEST(CodecTest, DictionaryFieldTransfersValueType) {
    // A dictionary field may be supplied as a plain value-type scalar; it
    // round-trips to a plain value scalar (indices are not on the wire).
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({arrow::field("v", dict_type, /*nullable=*/true)});
    fletcher::Codec codec(schema);

    auto value = std::make_shared<arrow::StringScalar>("alpha");
    auto decoded = codec.DecodeRow(codec.EncodeRow({value}));
    ASSERT_EQ(decoded.size(), 1u);
    ASSERT_TRUE(decoded[0]->is_valid);
    EXPECT_EQ(decoded[0]->type->id(), arrow::Type::STRING);
    EXPECT_TRUE(decoded[0]->Equals(*value));
}

TEST(CodecTest, DictionaryScalarEncodesResolvedValue) {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({arrow::field("v", dict_type, /*nullable=*/true)});
    fletcher::Codec codec(schema);

    // Build a DictionaryScalar that points at "z".
    arrow::StringBuilder vb;
    ASSERT_TRUE(vb.AppendValues({"x", "y", "z"}).ok());
    auto dict_values = vb.Finish().ValueOrDie();
    arrow::Int32Builder ib;
    ASSERT_TRUE(ib.Append(2).ok());
    auto indices = ib.Finish().ValueOrDie();
    auto dict_arr =
        arrow::DictionaryArray::FromArrays(dict_type, indices, dict_values).ValueOrDie();
    auto ds = dict_arr->GetScalar(0).ValueOrDie();

    auto decoded = codec.DecodeRow(codec.EncodeRow({ds}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(arrow::StringScalar("z")));
}

TEST(CodecTest, DictionaryNestedValueTypeRejected) {
    // Nested dictionary value types are not supported and must error clearly.
    auto nested = arrow::dictionary(arrow::int32(), arrow::list(arrow::int32()));
    auto schema = arrow::schema({arrow::field("v", nested, /*nullable=*/true)});
    fletcher::Codec codec(schema);

    // One field, marked non-null (0x00 bitfield): decode reaches the guard.
    const std::vector<uint8_t> buf = {0x00};
    EXPECT_THROW(static_cast<void>(codec.DecodeRow(buf.data(), buf.size())), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Malformed-input / safety (Phase 1 hardening)
// ---------------------------------------------------------------------------

TEST(CodecTest, DecodeRejectsTrailingBytes) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    fletcher::Codec codec(schema);
    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(7)});
    row.push_back(0xAB);  // padding / corruption after a valid row
    EXPECT_THROW(static_cast<void>(codec.DecodeRow(row)), std::invalid_argument);
}

TEST(CodecTest, DecodeRejectsTruncatedBuffer) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    fletcher::Codec codec(schema);
    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(7)});
    ASSERT_GT(row.size(), 1u);
    row.pop_back();  // one byte short
    EXPECT_THROW(static_cast<void>(codec.DecodeRow(row)), std::invalid_argument);
}

TEST(CodecTest, DecodeRejectsOversizedListCount) {
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::int32()))});
    fletcher::Codec codec(schema);
    // [row bitfield = 0x00 (field present)] [list COUNT = 0xFFFFFFFF] and nothing else.
    const std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(static_cast<void>(codec.DecodeRow(buf.data(), buf.size())), std::invalid_argument);
}

TEST(CodecTest, DecodeRejectsOversizedMapCount) {
    auto schema = arrow::schema({arrow::field("v", arrow::map(arrow::utf8(), arrow::int32()))});
    fletcher::Codec codec(schema);
    const std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(static_cast<void>(codec.DecodeRow(buf.data(), buf.size())), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Map type fidelity (decode preserves the schema's map type, not a default)
// ---------------------------------------------------------------------------

TEST(CodecTest, MapDecodePreservesKeysSortedType) {
    auto map_type = arrow::map(arrow::utf8(), arrow::int32(), /*keys_sorted=*/true);
    auto schema = arrow::schema({arrow::field("m", map_type)});
    fletcher::Codec codec(schema);

    arrow::StringBuilder kb;
    ASSERT_TRUE(kb.Append("a").ok());
    auto keys = kb.Finish().ValueOrDie();
    arrow::Int32Builder vb;
    ASSERT_TRUE(vb.Append(1).ok());
    auto vals = vb.Finish().ValueOrDie();
    // Build the entries struct from the schema's own entries type.
    const auto& mt = static_cast<const arrow::MapType&>(*map_type);
    auto entries = arrow::StructArray::Make({keys, vals}, mt.value_type()->fields()).ValueOrDie();
    std::shared_ptr<arrow::Scalar> map_scalar =
        std::make_shared<arrow::MapScalar>(entries, map_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({map_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    // The decoded scalar keeps the schema's keys_sorted=true map type rather
    // than a reconstructed keys_sorted=false default.
    EXPECT_TRUE(decoded[0]->type->Equals(*map_type));
}

// ---------------------------------------------------------------------------
// Sparse union (first round-trip coverage; only the active variant survives)
// ---------------------------------------------------------------------------

TEST(CodecTest, SparseUnionRoundtripActiveVariant) {
    auto union_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type)});
    fletcher::Codec codec(schema);

    // Active variant: the string child (field index 1). FromValue fills the
    // inactive children with null scalars, which is exactly what decode rebuilds.
    auto active = std::make_shared<arrow::StringScalar>("hi");
    auto union_scalar = arrow::SparseUnionScalar::FromValue(active, /*field_index=*/1, union_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({union_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*union_scalar));
}

// ---------------------------------------------------------------------------
// Trailing `_ingest_offset` system column — the RowBatcher pattern
// ---------------------------------------------------------------------------
//
// A producer encodes a DATA row, then splices a trailing UInt64 `_ingest_offset`
// onto it with AppendTrailingUint64Field (no re-encode). A consumer reads the offset
// back two ways and they agree:
//   * WITH A SCHEMA — decode the row against the *registered* schema (data fields +
//     the trailing `_ingest_offset`); the offset is then simply the last field of
//     the decoded ArrowRow. This is the path a subscriber uses (the registered
//     schema travels on the provider's companion `__schema` topic).
//   * No schema — the O(1) fast path ReadTrailingUint64Field (last 8 bytes).

namespace {

std::shared_ptr<arrow::Schema> DataSchema() {
    return arrow::schema({
        arrow::field("a", arrow::int32(), /*nullable=*/true),
        arrow::field("b", arrow::utf8(), /*nullable=*/true),
    });
}

// Registered schema = data schema + a trailing, non-null `_ingest_offset` UInt64.
std::shared_ptr<arrow::Schema> RegisteredSchema() {
    return arrow::schema({
        arrow::field("a", arrow::int32(), /*nullable=*/true),
        arrow::field("b", arrow::utf8(), /*nullable=*/true),
        arrow::field("_ingest_offset", arrow::uint64(), /*nullable=*/false),
    });
}

}  // namespace

TEST(IngestOffsetTrailingField, SchemaDecodeAndFastPathAgree) {
    // Producer: encode the DATA row, then splice the offset on (no re-encode).
    fletcher::Codec data_codec(DataSchema());
    fletcher::EncodedRow data_bytes = data_codec.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(7),
        std::make_shared<arrow::StringScalar>("hello positional"),
    });

    const uint64_t ingest_offset = 1234567890123ULL;
    auto registered_bytes = fletcher::AppendTrailingUint64Field(
        data_bytes, /*base_num_fields=*/DataSchema()->num_fields(), ingest_offset);

    // Consumer, WITH A SCHEMA: decode against the registered schema; `_ingest_offset`
    // is the last field of the ArrowRow.
    fletcher::ArrowRow decoded = fletcher::Codec(RegisteredSchema()).DecodeRow(registered_bytes);
    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_TRUE(decoded[0]->Equals(arrow::Int32Scalar(7)));
    EXPECT_TRUE(decoded[1]->Equals(arrow::StringScalar("hello positional")));
    ASSERT_EQ(decoded.back()->type->id(), arrow::Type::UINT64);
    const uint64_t offset_via_schema =
        std::static_pointer_cast<arrow::UInt64Scalar>(decoded.back())->value;
    EXPECT_EQ(offset_via_schema, ingest_offset);

    // Consumer, no schema: O(1) trailing read.
    const uint64_t offset_via_fast_path = fletcher::ReadTrailingUint64Field(registered_bytes);
    EXPECT_EQ(offset_via_fast_path, ingest_offset);

    // Both paths agree by construction.
    EXPECT_EQ(offset_via_schema, offset_via_fast_path);
}

TEST(IngestOffsetTrailingField, PreservesNullDataFieldUnderSchemaDecode) {
    fletcher::Codec data_codec(DataSchema());
    fletcher::EncodedRow data_bytes = data_codec.EncodeRow({
        arrow::MakeNullScalar(arrow::int32()),             // field "a" = null
        std::make_shared<arrow::StringScalar>("payload"),  // field "b"
    });

    const uint64_t ingest_offset = 42;
    auto registered_bytes =
        fletcher::AppendTrailingUint64Field(data_bytes, DataSchema()->num_fields(), ingest_offset);

    fletcher::ArrowRow decoded = fletcher::Codec(RegisteredSchema()).DecodeRow(registered_bytes);
    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_FALSE(decoded[0]->is_valid);  // the data null survived the splice
    EXPECT_TRUE(decoded[1]->Equals(arrow::StringScalar("payload")));
    EXPECT_EQ(std::static_pointer_cast<arrow::UInt64Scalar>(decoded.back())->value, ingest_offset);
    EXPECT_EQ(fletcher::ReadTrailingUint64Field(registered_bytes), ingest_offset);
}
