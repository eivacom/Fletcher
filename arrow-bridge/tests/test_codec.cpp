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
#include <functional>
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

TEST(CodecTest, LargePrimitiveListRoundtripsThroughTheRunPath) {
    // 2 667 floats = one point-cloud row under the 64 KiB DDS bound: the bulk path must produce
    // exactly what the per-element path did, for every fixed-width element type, and a single null
    // must send the list back down the scalar path
    for (auto elem : {arrow::float32(), arrow::float64(), arrow::uint32(), arrow::int64(),
                      arrow::uint8(), arrow::int16()}) {
        auto schema = arrow::schema({arrow::field("xyz", arrow::list(elem))});
        fletcher::Codec codec(schema);
        std::unique_ptr<arrow::ArrayBuilder> builder;
        ASSERT_TRUE(arrow::MakeBuilder(arrow::default_memory_pool(), elem, &builder).ok());
        for (int i = 0; i < 2667 * 3; ++i) {
            auto scalar = arrow::MakeScalar(elem, i % 250).ValueOrDie();
            ASSERT_TRUE(builder->AppendScalar(*scalar).ok());
        }
        auto arr = builder->Finish().ValueOrDie();
        auto list_scalar = std::make_shared<arrow::ListScalar>(arr);
        auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
        ASSERT_EQ(decoded.size(), 1u);
        EXPECT_TRUE(decoded[0]->Equals(*list_scalar)) << elem->ToString();
    }
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::float32()))});
    fletcher::Codec codec(schema);
    arrow::FloatBuilder fb;
    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE((i == 57 ? fb.AppendNull() : fb.Append(static_cast<float>(i))).ok());
    auto with_null = std::make_shared<arrow::ListScalar>(fb.Finish().ValueOrDie());
    auto decoded = codec.DecodeRow(codec.EncodeRow({with_null}));
    EXPECT_TRUE(decoded[0]->Equals(*with_null));
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

// ---------------------------------------------------------------------------
// Oracle tests: Codec::EncodeRow bytes must match a hand-built encoding of
// the same values using fletcher::PositionalWriter over a VectorWriteBuffer,
// per the wire format comment in codec.hpp. These pin the wire format
// independently of the Codec implementation.
// ---------------------------------------------------------------------------

TEST(CodecTest, EncodeMatchesPositionalWriter_Scalars10) {
    auto schema = arrow::schema({
        arrow::field("f0", arrow::boolean(), true),
        arrow::field("f1", arrow::int32(), true),
        arrow::field("f2", arrow::int64(), true),
        arrow::field("f3", arrow::uint32(), true),
        arrow::field("f4", arrow::float32(), true),
        arrow::field("f5", arrow::float64(), true),
        arrow::field("f6", arrow::utf8(), true),
        arrow::field("f7", arrow::timestamp(arrow::TimeUnit::NANO), true),
        arrow::field("f8", arrow::duration(arrow::TimeUnit::NANO), true),
        arrow::field("f9", arrow::binary(), true),
    });
    fletcher::Codec codec(schema);

    const bool v0 = true;
    const int32_t v1 = -1'000'000;
    const int64_t v2 = 9'223'372'036'854'775'807LL;
    const uint32_t v3 = 4'294'967'295u;
    const float v4 = 3.14f;
    const double v5 = 2.718281828;
    const std::string v6 = "scalars10";
    const int64_t v7 = 1'234'567'890'123'456'789LL;
    const int64_t v8 = 987'654'321LL;
    const std::vector<uint8_t> v9 = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};

    fletcher::ArrowRow row = {
        std::make_shared<arrow::BooleanScalar>(v0),
        std::make_shared<arrow::Int32Scalar>(v1),
        std::make_shared<arrow::Int64Scalar>(v2),
        std::make_shared<arrow::UInt32Scalar>(v3),
        std::make_shared<arrow::FloatScalar>(v4),
        std::make_shared<arrow::DoubleScalar>(v5),
        std::make_shared<arrow::StringScalar>(v6),
        std::make_shared<arrow::TimestampScalar>(v7, schema->field(7)->type()),
        std::make_shared<arrow::DurationScalar>(v8, schema->field(8)->type()),
        std::make_shared<arrow::BinaryScalar>(
            std::make_shared<arrow::Buffer>(v9.data(), static_cast<int64_t>(v9.size()))),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 10);
    pw.WriteBool(v0);
    pw.WriteInt32(v1);
    pw.WriteInt64(v2);
    pw.WriteUint32(v3);
    pw.WriteFloat(v4);
    pw.WriteDouble(v5);
    pw.WriteString(v6);
    pw.WriteTimestamp(v7);
    pw.WriteDuration(v8);
    pw.WriteBinary(v9.data(), v9.size());
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Nullable10) {
    auto schema = arrow::schema({
        arrow::field("f0", arrow::boolean(), true),
        arrow::field("f1", arrow::int32(), true),
        arrow::field("f2", arrow::int64(), true),
        arrow::field("f3", arrow::uint32(), true),
        arrow::field("f4", arrow::float32(), true),
        arrow::field("f5", arrow::float64(), true),
        arrow::field("f6", arrow::utf8(), true),
        arrow::field("f7", arrow::timestamp(arrow::TimeUnit::NANO), true),
        arrow::field("f8", arrow::duration(arrow::TimeUnit::NANO), true),
        arrow::field("f9", arrow::binary(), true),
    });
    fletcher::Codec codec(schema);

    const int32_t v1 = 17;
    const int64_t v2 = -8'000'000'000LL;
    const float v4 = 1.5f;
    const double v5 = -6.25;
    const int64_t v7 = 42LL;
    const int64_t v8 = 99LL;

    fletcher::ArrowRow row = {
        arrow::MakeNullScalar(arrow::boolean()),
        std::make_shared<arrow::Int32Scalar>(v1),
        std::make_shared<arrow::Int64Scalar>(v2),
        arrow::MakeNullScalar(arrow::uint32()),
        std::make_shared<arrow::FloatScalar>(v4),
        std::make_shared<arrow::DoubleScalar>(v5),
        arrow::MakeNullScalar(arrow::utf8()),
        std::make_shared<arrow::TimestampScalar>(v7, schema->field(7)->type()),
        std::make_shared<arrow::DurationScalar>(v8, schema->field(8)->type()),
        arrow::MakeNullScalar(arrow::binary()),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 10);
    pw.SetNull(0);
    pw.SetNull(3);
    pw.SetNull(6);
    pw.SetNull(9);
    pw.WriteInt32(v1);
    pw.WriteInt64(v2);
    pw.WriteFloat(v4);
    pw.WriteDouble(v5);
    pw.WriteTimestamp(v7);
    pw.WriteDuration(v8);
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Cloud) {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    auto schema = arrow::schema({
        arrow::field("t", ts_type, true),
        arrow::field("xs", arrow::list(arrow::float32()), true),
        arrow::field("ys", arrow::list(arrow::uint32()), true),
    });
    fletcher::Codec codec(schema);

    const int64_t ts = 123456789LL;
    constexpr int kCount = 2667;
    std::vector<float> xs(kCount);
    std::vector<uint32_t> ys(kCount);
    for (int i = 0; i < kCount; ++i) {
        xs[static_cast<size_t>(i)] = static_cast<float>(i) * 0.5f;
        ys[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
    }

    arrow::FloatBuilder xb;
    ASSERT_TRUE(xb.AppendValues(xs).ok());
    auto xs_arr = xb.Finish().ValueOrDie();
    arrow::UInt32Builder yb;
    ASSERT_TRUE(yb.AppendValues(ys).ok());
    auto ys_arr = yb.Finish().ValueOrDie();

    fletcher::ArrowRow row = {
        std::make_shared<arrow::TimestampScalar>(ts, ts_type),
        std::make_shared<arrow::ListScalar>(xs_arr, schema->field(1)->type()),
        std::make_shared<arrow::ListScalar>(ys_arr, schema->field(2)->type()),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 3);
    pw.WriteTimestamp(ts);
    pw.BeginList(static_cast<uint32_t>(kCount));
    pw.WriteFixedArray(xs.data(), xs.size());
    pw.BeginList(static_cast<uint32_t>(kCount));
    pw.WriteFixedArray(ys.data(), ys.size());
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_ListWithNullElement) {
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::float32()), true)});
    fletcher::Codec codec(schema);

    constexpr int kCount = 100;
    constexpr int kNullIndex = 57;
    arrow::FloatBuilder fb;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE((i == kNullIndex ? fb.AppendNull() : fb.Append(static_cast<float>(i))).ok());
    }
    auto arr = fb.Finish().ValueOrDie();

    fletcher::ArrowRow row = {
        std::make_shared<arrow::ListScalar>(arr, schema->field(0)->type()),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 1);
    auto lc = pw.BeginList(static_cast<uint32_t>(kCount));
    lc.SetElementNull(static_cast<uint32_t>(kNullIndex));
    for (int i = 0; i < kCount; ++i) {
        if (i == kNullIndex) continue;
        pw.WriteFloat(static_cast<float>(i));
    }
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Pose) {
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    auto matrix_type = arrow::struct_({arrow::field("m", arrow::list(arrow::float64()), true)});
    auto cov_type = arrow::struct_({arrow::field("c", arrow::list(arrow::float64()), true)});
    auto schema = arrow::schema({
        arrow::field("t", ts_type, true),
        arrow::field("orientation", matrix_type, true),
        arrow::field("covariance", cov_type, true),
    });
    fletcher::Codec codec(schema);

    const int64_t ts = 55LL;
    std::vector<double> matrix(16);
    for (int i = 0; i < 16; ++i) matrix[static_cast<size_t>(i)] = i * 1.5;
    std::vector<double> cov(6);
    for (int i = 0; i < 6; ++i) cov[static_cast<size_t>(i)] = i + 0.25;

    arrow::DoubleBuilder mb;
    ASSERT_TRUE(mb.AppendValues(matrix).ok());
    auto matrix_arr = mb.Finish().ValueOrDie();
    arrow::DoubleBuilder cb;
    ASSERT_TRUE(cb.AppendValues(cov).ok());
    auto cov_arr = cb.Finish().ValueOrDie();

    auto matrix_list_type = static_cast<const arrow::StructType&>(*matrix_type).field(0)->type();
    auto cov_list_type = static_cast<const arrow::StructType&>(*cov_type).field(0)->type();

    auto orientation_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{std::make_shared<arrow::ListScalar>(matrix_arr, matrix_list_type)},
        matrix_type);
    auto covariance_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{std::make_shared<arrow::ListScalar>(cov_arr, cov_list_type)}, cov_type);

    fletcher::ArrowRow row = {
        std::make_shared<arrow::TimestampScalar>(ts, ts_type),
        orientation_scalar,
        covariance_scalar,
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 3);
    pw.WriteTimestamp(ts);
    {
        auto sw = pw.BeginStruct(1);
        sw.BeginList(static_cast<uint32_t>(matrix.size()));
        sw.WriteFixedArray(matrix.data(), matrix.size());
    }
    {
        auto sw = pw.BeginStruct(1);
        sw.BeginList(static_cast<uint32_t>(cov.size()));
        sw.WriteFixedArray(cov.data(), cov.size());
    }
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Points) {
    auto point_type = arrow::struct_({
        arrow::field("x", arrow::float64(), true),
        arrow::field("y", arrow::float64(), true),
        arrow::field("z", arrow::float64(), true),
    });
    auto list_type = arrow::list(point_type);
    auto schema = arrow::schema({arrow::field("points", list_type, true)});
    fletcher::Codec codec(schema);

    struct Pt {
        double x, y, z;
    };
    std::vector<Pt> pts = {{1.0, 2.0, 3.0}, {-1.5, 0.5, 9.25}, {100.0, -200.0, 0.0}};

    arrow::DoubleBuilder xb, yb, zb;
    for (const auto& p : pts) {
        ASSERT_TRUE(xb.Append(p.x).ok());
        ASSERT_TRUE(yb.Append(p.y).ok());
        ASSERT_TRUE(zb.Append(p.z).ok());
    }
    auto x_arr = xb.Finish().ValueOrDie();
    auto y_arr = yb.Finish().ValueOrDie();
    auto z_arr = zb.Finish().ValueOrDie();
    auto structs =
        arrow::StructArray::Make({x_arr, y_arr, z_arr}, point_type->fields()).ValueOrDie();

    fletcher::ArrowRow row = {
        std::make_shared<arrow::ListScalar>(structs, list_type),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 1);
    pw.BeginList(static_cast<uint32_t>(pts.size()));
    for (const auto& p : pts) {
        auto sw = pw.BeginStruct(3);
        sw.WriteDouble(p.x);
        sw.WriteDouble(p.y);
        sw.WriteDouble(p.z);
    }
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Nested) {
    // list<list<struct<x,y:double>>>, 2 outer elements x 2 inner elements each.
    auto xy_type = arrow::struct_({
        arrow::field("x", arrow::float64(), true),
        arrow::field("y", arrow::float64(), true),
    });
    auto inner_list_type = arrow::list(xy_type);
    auto outer_list_type = arrow::list(inner_list_type);

    // map<utf8, double>, 2 entries.
    auto map_type = arrow::map(arrow::utf8(), arrow::float64());

    // struct{struct{int32, utf8}}
    auto leaf_struct_type = arrow::struct_({
        arrow::field("a", arrow::int32(), true),
        arrow::field("b", arrow::utf8(), true),
    });
    auto outer_struct_type = arrow::struct_({arrow::field("inner", leaf_struct_type, true)});

    auto schema = arrow::schema({
        arrow::field("nested_list", outer_list_type, true),
        arrow::field("m", map_type, true),
        arrow::field("s", outer_struct_type, true),
    });
    fletcher::Codec codec(schema);

    // --- Build the list<list<struct<x,y>>> value. ---
    const double xs[4] = {1.0, 2.0, 3.0, 4.0};
    const double ys[4] = {10.0, 20.0, 30.0, 40.0};
    arrow::DoubleBuilder xb, yb;
    ASSERT_TRUE(xb.AppendValues(xs, 4).ok());
    ASSERT_TRUE(yb.AppendValues(ys, 4).ok());
    auto x_arr = xb.Finish().ValueOrDie();
    auto y_arr = yb.Finish().ValueOrDie();
    auto structs = arrow::StructArray::Make({x_arr, y_arr}, xy_type->fields()).ValueOrDie();

    arrow::Int32Builder offsets_b;
    ASSERT_TRUE(offsets_b.AppendValues(std::vector<int32_t>{0, 2, 4}).ok());
    auto offsets_arr = offsets_b.Finish().ValueOrDie();
    auto inner_list_arr = arrow::ListArray::FromArrays(*offsets_arr, *structs).ValueOrDie();

    auto nested_list_scalar = std::make_shared<arrow::ListScalar>(inner_list_arr, outer_list_type);

    // --- Build the map<utf8,double> value. ---
    arrow::StringBuilder kb;
    ASSERT_TRUE(kb.Append("k0").ok());
    ASSERT_TRUE(kb.Append("k1").ok());
    auto keys = kb.Finish().ValueOrDie();
    arrow::DoubleBuilder vb;
    ASSERT_TRUE(vb.Append(1.5).ok());
    ASSERT_TRUE(vb.Append(-2.5).ok());
    auto vals = vb.Finish().ValueOrDie();
    const auto& mt = static_cast<const arrow::MapType&>(*map_type);
    auto entries = arrow::StructArray::Make({keys, vals}, mt.value_type()->fields()).ValueOrDie();
    auto map_scalar = std::make_shared<arrow::MapScalar>(entries, map_type);

    // --- Build the struct{struct{int32, utf8}} value. ---
    auto leaf_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{
            std::make_shared<arrow::Int32Scalar>(7),
            std::make_shared<arrow::StringScalar>("leaf"),
        },
        leaf_struct_type);
    auto outer_struct_scalar =
        std::make_shared<arrow::StructScalar>(arrow::ScalarVector{leaf_scalar}, outer_struct_type);

    fletcher::ArrowRow row = {nested_list_scalar, map_scalar, outer_struct_scalar};
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 3);

    // list<list<struct<x,y>>>
    pw.BeginList(2);
    for (int o = 0; o < 2; ++o) {
        pw.BeginList(2);
        for (int j = 0; j < 2; ++j) {
            auto sw = pw.BeginStruct(2);
            sw.WriteDouble(xs[o * 2 + j]);
            sw.WriteDouble(ys[o * 2 + j]);
        }
    }

    // map<utf8,double>
    {
        auto mc = pw.BeginMap(2);
        pw.WriteString("k0");
        pw.WriteString("k1");
        mc.BeginValues();
        pw.WriteDouble(1.5);
        pw.WriteDouble(-2.5);
    }

    // struct{struct{int32,utf8}}
    {
        auto sw_outer = pw.BeginStruct(1);
        auto sw_inner = sw_outer.BeginStruct(2);
        sw_inner.WriteInt32(7);
        sw_inner.WriteString("leaf");
    }

    auto hand_bytes = buf.Finish();
    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_FixedSizeList) {
    auto fsl_type = arrow::fixed_size_list(arrow::float32(), 3);
    auto schema = arrow::schema({arrow::field("v", fsl_type, true)});
    fletcher::Codec codec(schema);

    const std::vector<float> values = {1.0f, -2.0f, 3.5f};
    arrow::FloatBuilder fb;
    ASSERT_TRUE(fb.AppendValues(values).ok());
    auto arr = fb.Finish().ValueOrDie();

    fletcher::ArrowRow row = {
        std::make_shared<arrow::FixedSizeListScalar>(arr, fsl_type),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 1);
    size_t off = buf.Position();
    buf.AppendZeros(1);
    [[maybe_unused]] fletcher::PositionalWriter::ListContext lc{buf, off, 3};
    pw.WriteFixedArray(values.data(), values.size());
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_DecimalAndIntervals) {
    auto dec_type = arrow::decimal128(10, 2);
    auto schema = arrow::schema({
        arrow::field("d", dec_type, true),
        arrow::field("months", arrow::month_interval(), true),
        arrow::field("daytime", arrow::day_time_interval(), true),
        arrow::field("mdn", arrow::month_day_nano_interval(), true),
    });
    fletcher::Codec codec(schema);

    arrow::Decimal128 dec_val(12345);
    const int32_t months = 5;
    arrow::DayTimeIntervalType::DayMilliseconds dt{3, 1000};
    arrow::MonthDayNanoIntervalType::MonthDayNanos mdn{2, 5, 123456789LL};

    fletcher::ArrowRow row = {
        std::make_shared<arrow::Decimal128Scalar>(dec_val, dec_type),
        std::make_shared<arrow::MonthIntervalScalar>(months),
        std::make_shared<arrow::DayTimeIntervalScalar>(dt),
        std::make_shared<arrow::MonthDayNanoIntervalScalar>(mdn),
    };
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    [[maybe_unused]] fletcher::PositionalWriter pw(buf, 4);
    {
        uint8_t bytes[16];
        dec_val.ToBytes(bytes);
        buf.Append(bytes, 16);
    }
    buf.AppendFixed<int32_t>(months);
    buf.AppendFixed<int32_t>(dt.days);
    buf.AppendFixed<int32_t>(dt.milliseconds);
    buf.AppendFixed<int32_t>(mdn.months);
    buf.AppendFixed<int32_t>(mdn.days);
    buf.AppendFixed<int64_t>(mdn.nanoseconds);
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Unions) {
    auto dense_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto sparse_type = arrow::sparse_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({
        arrow::field("du", dense_type, true),
        arrow::field("su", sparse_type, true),
    });
    fletcher::Codec codec(schema);

    auto dense_val = std::make_shared<arrow::StringScalar>("hi-dense");
    auto dense_scalar =
        std::make_shared<arrow::DenseUnionScalar>(dense_val, /*type_code=*/1, dense_type);
    auto sparse_val = std::make_shared<arrow::Int32Scalar>(99);
    auto sparse_scalar =
        arrow::SparseUnionScalar::FromValue(sparse_val, /*field_index=*/0, sparse_type);

    fletcher::ArrowRow row = {dense_scalar, sparse_scalar};
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 2);
    buf.AppendFixed<int8_t>(1);
    pw.WriteString("hi-dense");
    buf.AppendFixed<int8_t>(0);
    pw.WriteInt32(99);
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

// ---------------------------------------------------------------------------
// Red-first negative tests (Step 1b).
// ---------------------------------------------------------------------------

TEST(CodecTest, FixedSizeListDecodeKeepsSchemaType) {
    auto fsl_type = arrow::fixed_size_list(arrow::field("v", arrow::float32(), false), 3);
    auto schema = arrow::schema({arrow::field("vec", fsl_type)});
    fletcher::Codec codec(schema);

    auto builder = arrow::FloatBuilder();
    ASSERT_TRUE(builder.AppendValues({1.0f, 2.0f, 3.0f}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto fsl_scalar = std::make_shared<arrow::FixedSizeListScalar>(arr, fsl_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({fsl_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->type->Equals(*schema->field(0)->type()));
}

TEST(CodecTest, EncodeRejectsTimestampUnitMismatch) {
    auto schema = arrow::schema({arrow::field("t", arrow::timestamp(arrow::TimeUnit::MILLI))});
    fletcher::Codec codec(schema);
    auto scalar =
        std::make_shared<arrow::TimestampScalar>(123LL, arrow::timestamp(arrow::TimeUnit::NANO));
    EXPECT_THROW(static_cast<void>(codec.EncodeRow({scalar})), std::invalid_argument);
}

TEST(CodecTest, EncodeRejectsListElementTypeMismatch) {
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::int64()))});
    fletcher::Codec codec(schema);

    arrow::Int32Builder builder;
    ASSERT_TRUE(builder.AppendValues({1, 2, 3}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr);  // list<int32>, not list<int64>

    EXPECT_THROW(static_cast<void>(codec.EncodeRow({list_scalar})), std::invalid_argument);
}

TEST(CodecTest, UnionDecodeRejectsUnknownTypeCode) {
    auto union_type = arrow::dense_union(
        {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type)});
    fletcher::Codec codec(schema);

    auto union_scalar = std::make_shared<arrow::DenseUnionScalar>(
        std::make_shared<arrow::Int32Scalar>(1), 0, union_type);
    auto encoded = codec.EncodeRow({union_scalar});
    // Byte layout: [row bitfield: 1 byte][type_code: 1 byte][payload...]. Patch the type code to
    // an unknown value.
    ASSERT_GT(encoded.size(), 1u);
    encoded[1] = 100;
    EXPECT_THROW(static_cast<void>(codec.DecodeRow(encoded)), std::invalid_argument);
}

// Red-first (crash): today GetEncodedValue() succeeds with an is_valid=false scalar for a null
// dictionary entry, and EncodeScalar's STRING case dereferences its null value buffer -> SEGV.
TEST(CodecTest, DictionaryEncodeRejectsNullEntry) {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({arrow::field("v", dict_type)});
    fletcher::Codec codec(schema);

    arrow::StringBuilder vb;
    ASSERT_TRUE(vb.Append("x").ok());
    ASSERT_TRUE(vb.AppendNull().ok());
    std::shared_ptr<arrow::Array> dict_values;
    ASSERT_TRUE(vb.Finish(&dict_values).ok());

    arrow::Int32Builder ib;
    ASSERT_TRUE(ib.Append(1).ok());  // index 1 -> the null dictionary entry
    std::shared_ptr<arrow::Array> indices;
    ASSERT_TRUE(ib.Finish(&indices).ok());

    auto dict_arr =
        arrow::DictionaryArray::FromArrays(dict_type, indices, dict_values).ValueOrDie();
    auto ds = dict_arr->GetScalar(0).ValueOrDie();

    EXPECT_THROW(static_cast<void>(codec.EncodeRow({ds})), std::invalid_argument);
}

// Red-first (crash): today the map key loop calls GetScalar on a null key and EncodeScalar's
// STRING case dereferences its null value buffer -> SEGV.
TEST(CodecTest, MapEncodeRejectsNullKey) {
    auto map_type = arrow::map(arrow::utf8(), arrow::int32());
    auto schema = arrow::schema({arrow::field("m", map_type)});
    fletcher::Codec codec(schema);

    arrow::StringBuilder kb;
    ASSERT_TRUE(kb.Append("a").ok());
    ASSERT_TRUE(kb.AppendNull().ok());
    std::shared_ptr<arrow::Array> keys;
    ASSERT_TRUE(kb.Finish(&keys).ok());

    arrow::Int32Builder vb2;
    ASSERT_TRUE(vb2.Append(1).ok());
    ASSERT_TRUE(vb2.Append(2).ok());
    std::shared_ptr<arrow::Array> vals;
    ASSERT_TRUE(vb2.Finish(&vals).ok());

    const auto& mt = static_cast<const arrow::MapType&>(*map_type);
    auto entries = arrow::StructArray::Make({keys, vals}, mt.value_type()->fields()).ValueOrDie();
    auto map_scalar = std::make_shared<arrow::MapScalar>(entries, map_type);

    EXPECT_THROW(static_cast<void>(codec.EncodeRow({map_scalar})), std::invalid_argument);
}

TEST(CodecTest, EncodeMatchesPositionalWriter_Dictionary) {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema = arrow::schema({arrow::field("v", dict_type, true)});
    fletcher::Codec codec(schema);

    const std::string value = "dict-value";
    fletcher::ArrowRow row = {std::make_shared<arrow::StringScalar>(value)};
    auto codec_bytes = codec.EncodeRow(row);

    fletcher::VectorWriteBuffer buf;
    fletcher::PositionalWriter pw(buf, 1);
    pw.WriteString(value);
    auto hand_bytes = buf.Finish();

    EXPECT_EQ(codec_bytes, hand_bytes);
}

// ---------------------------------------------------------------------------
// Gap-filler round-trip tests (Step 1c).
// ---------------------------------------------------------------------------

TEST(CodecTest, TemporalUnitsRoundtrip) {
    auto time32_type = arrow::time32(arrow::TimeUnit::SECOND);
    auto time64_type = arrow::time64(arrow::TimeUnit::MICRO);
    auto duration_type = arrow::duration(arrow::TimeUnit::MILLI);
    auto ts_type = arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
    auto schema = arrow::schema({
        arrow::field("t32", time32_type, true),
        arrow::field("t64", time64_type, true),
        arrow::field("dur", duration_type, true),
        arrow::field("ts", ts_type, true),
    });
    fletcher::Codec codec(schema);

    fletcher::ArrowRow row = {
        std::make_shared<arrow::Time32Scalar>(12345, time32_type),
        std::make_shared<arrow::Time64Scalar>(123456789LL, time64_type),
        std::make_shared<arrow::DurationScalar>(9876LL, duration_type),
        std::make_shared<arrow::TimestampScalar>(1'700'000'000'000'000LL, ts_type),
    };
    auto decoded = codec.DecodeRow(codec.EncodeRow(row));
    ASSERT_EQ(decoded.size(), 4u);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(decoded[i]->Equals(*row[i]));
        EXPECT_TRUE(decoded[i]->type->Equals(*schema->field(i)->type()));
    }
}

TEST(CodecTest, Decimal256Roundtrip) {
    auto dec_type = arrow::decimal256(20, 4);
    CHECK_POS_RT(dec_type, std::make_shared<arrow::Decimal256Scalar>(arrow::Decimal256(123456789LL),
                                                                     dec_type));
}

TEST(CodecTest, IntervalTypesRoundtrip) {
    CHECK_POS_RT(arrow::month_interval(), std::make_shared<arrow::MonthIntervalScalar>(7));
    CHECK_POS_RT(arrow::day_time_interval(),
                 std::make_shared<arrow::DayTimeIntervalScalar>(
                     arrow::DayTimeIntervalType::DayMilliseconds{3, 456}));
    CHECK_POS_RT(arrow::month_day_nano_interval(),
                 std::make_shared<arrow::MonthDayNanoIntervalScalar>(
                     arrow::MonthDayNanoIntervalType::MonthDayNanos{1, 2, 3}));
}

TEST(CodecTest, LargeListRoundtrip) {
    auto list_type = arrow::large_list(arrow::int32());
    auto schema = arrow::schema({arrow::field("v", list_type, true)});
    fletcher::Codec codec(schema);

    arrow::Int32Builder builder;
    ASSERT_TRUE(builder.AppendValues({1, 2, 3, 4}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::LargeListScalar>(arr, list_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

TEST(CodecTest, ListOfListRoundtrip) {
    // list<list<int32>> with an empty inner list and a null inner list.
    auto inner_type = arrow::list(arrow::int32());
    auto outer_type = arrow::list(inner_type);
    auto schema = arrow::schema({arrow::field("v", outer_type, true)});
    fletcher::Codec codec(schema);

    auto value_builder = std::make_shared<arrow::Int32Builder>();
    arrow::ListBuilder list_builder(arrow::default_memory_pool(), value_builder);

    ASSERT_TRUE(list_builder.Append().ok());  // element 0: [1, 2]
    ASSERT_TRUE(value_builder->Append(1).ok());
    ASSERT_TRUE(value_builder->Append(2).ok());

    ASSERT_TRUE(list_builder.Append().ok());  // element 1: [] (empty, non-null)

    ASSERT_TRUE(list_builder.AppendNull().ok());  // element 2: null

    std::shared_ptr<arrow::Array> inner_lists_arr;
    ASSERT_TRUE(list_builder.Finish(&inner_lists_arr).ok());

    auto list_scalar = std::make_shared<arrow::ListScalar>(inner_lists_arr, outer_type);
    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

TEST(CodecTest, ListOfStructRoundtrip) {
    // list<struct<int32, utf8>> with one null struct element.
    auto struct_type = arrow::struct_({
        arrow::field("a", arrow::int32(), true),
        arrow::field("b", arrow::utf8(), true),
    });
    auto list_type = arrow::list(struct_type);
    auto schema = arrow::schema({arrow::field("v", list_type, true)});
    fletcher::Codec codec(schema);

    arrow::StructBuilder struct_builder(
        struct_type, arrow::default_memory_pool(),
        std::vector<std::shared_ptr<arrow::ArrayBuilder>>{
            std::make_shared<arrow::Int32Builder>(), std::make_shared<arrow::StringBuilder>()});
    auto* a_b = static_cast<arrow::Int32Builder*>(struct_builder.field_builder(0));
    auto* b_b = static_cast<arrow::StringBuilder*>(struct_builder.field_builder(1));

    ASSERT_TRUE(struct_builder.Append().ok());
    ASSERT_TRUE(a_b->Append(1).ok());
    ASSERT_TRUE(b_b->Append("x").ok());

    ASSERT_TRUE(struct_builder.AppendNull().ok());

    ASSERT_TRUE(struct_builder.Append().ok());
    ASSERT_TRUE(a_b->Append(3).ok());
    ASSERT_TRUE(b_b->Append("z").ok());

    std::shared_ptr<arrow::Array> arr;
    ASSERT_TRUE(struct_builder.Finish(&arr).ok());

    auto list_scalar = std::make_shared<arrow::ListScalar>(arr, list_type);
    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

TEST(CodecTest, StructWithListRoundtrip) {
    auto stype = arrow::struct_({
        arrow::field("id", arrow::int32(), true),
        arrow::field("items", arrow::list(arrow::utf8()), true),
    });
    auto schema = arrow::schema({arrow::field("s", stype, true)});
    fletcher::Codec codec(schema);

    arrow::StringBuilder items_builder;
    ASSERT_TRUE(items_builder.Append("a").ok());
    ASSERT_TRUE(items_builder.Append("bb").ok());
    auto items_arr = items_builder.Finish().ValueOrDie();

    auto struct_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{
            std::make_shared<arrow::Int32Scalar>(9),
            std::make_shared<arrow::ListScalar>(items_arr, stype->field(1)->type()),
        },
        stype);

    auto decoded = codec.DecodeRow(codec.EncodeRow({struct_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*struct_scalar));
}

TEST(CodecTest, SlicedArraysEncodeLikeUnsliced) {
    auto check = [](const std::shared_ptr<arrow::Array>& arr8,
                    const std::shared_ptr<arrow::Array>& arr4,
                    const std::shared_ptr<arrow::DataType>& elem_type) {
        auto list_type = arrow::list(elem_type);
        auto schema = arrow::schema({arrow::field("v", list_type, true)});
        fletcher::Codec codec(schema);

        auto sliced = arr8->Slice(3, 4);
        auto scalar_sliced = std::make_shared<arrow::ListScalar>(sliced, list_type);
        auto scalar_fresh = std::make_shared<arrow::ListScalar>(arr4, list_type);

        auto bytes_sliced = codec.EncodeRow({scalar_sliced});
        auto bytes_fresh = codec.EncodeRow({scalar_fresh});
        EXPECT_EQ(bytes_sliced, bytes_fresh);

        auto decoded = codec.DecodeRow(bytes_sliced);
        ASSERT_EQ(decoded.size(), 1u);
        EXPECT_TRUE(decoded[0]->Equals(*scalar_fresh));
    };

    // list<struct<int32, utf8>>
    {
        auto struct_type = arrow::struct_({
            arrow::field("a", arrow::int32(), true),
            arrow::field("b", arrow::utf8(), true),
        });
        auto build = [&](int start, int count) {
            arrow::StructBuilder sb(struct_type, arrow::default_memory_pool(),
                                    std::vector<std::shared_ptr<arrow::ArrayBuilder>>{
                                        std::make_shared<arrow::Int32Builder>(),
                                        std::make_shared<arrow::StringBuilder>()});
            auto* ab = static_cast<arrow::Int32Builder*>(sb.field_builder(0));
            auto* bb = static_cast<arrow::StringBuilder*>(sb.field_builder(1));
            for (int i = start; i < start + count; ++i) {
                EXPECT_TRUE(sb.Append().ok());
                EXPECT_TRUE(ab->Append(i).ok());
                EXPECT_TRUE(bb->Append("s" + std::to_string(i)).ok());
            }
            std::shared_ptr<arrow::Array> arr;
            EXPECT_TRUE(sb.Finish(&arr).ok());
            return arr;
        };
        check(build(0, 8), build(3, 4), struct_type);
    }

    // list<list<int32>>
    {
        auto inner_type = arrow::list(arrow::int32());
        auto build = [&](int start, int count) {
            auto values_builder = std::make_shared<arrow::Int32Builder>();
            arrow::ListBuilder lb(arrow::default_memory_pool(), values_builder);
            for (int i = start; i < start + count; ++i) {
                EXPECT_TRUE(lb.Append().ok());
                EXPECT_TRUE(values_builder->Append(i).ok());
                EXPECT_TRUE(values_builder->Append(i * 10).ok());
            }
            std::shared_ptr<arrow::Array> arr;
            EXPECT_TRUE(lb.Finish(&arr).ok());
            return arr;
        };
        check(build(0, 8), build(3, 4), inner_type);
    }

    // list<binary>
    {
        auto build = [&](int start, int count) {
            arrow::BinaryBuilder bb;
            for (int i = start; i < start + count; ++i) {
                EXPECT_TRUE(bb.Append("bin" + std::to_string(i)).ok());
            }
            std::shared_ptr<arrow::Array> arr;
            EXPECT_TRUE(bb.Finish(&arr).ok());
            return arr;
        };
        check(build(0, 8), build(3, 4), arrow::binary());
    }

    // list<decimal128>
    {
        auto dec_type = arrow::decimal128(12, 2);
        auto build = [&](int start, int count) {
            arrow::Decimal128Builder db(dec_type);
            for (int i = start; i < start + count; ++i) {
                EXPECT_TRUE(db.Append(arrow::Decimal128(i * 100)).ok());
            }
            std::shared_ptr<arrow::Array> arr;
            EXPECT_TRUE(db.Finish(&arr).ok());
            return arr;
        };
        check(build(0, 8), build(3, 4), dec_type);
    }
}

// ---------------------------------------------------------------------------
// Step 2: the shared run helper (AppendRun/FixedWidth in row_reader.hpp) must
// round-trip every fixed-width type it recognises, both as an all-valid run
// (bulk path) and with one null element (forces the per-element path).
// ---------------------------------------------------------------------------

TEST(CodecTest, EveryFixedWidthRunTypeRoundtrips) {
    constexpr int kCount = 300;
    constexpr int kNullIndex = 57;

    auto check = [](const std::shared_ptr<arrow::DataType>& elem_type,
                    const std::function<std::shared_ptr<arrow::Scalar>(int)>& gen) {
        auto schema = arrow::schema({arrow::field("v", arrow::list(elem_type), true)});
        fletcher::Codec codec(schema);

        for (int null_index : {-1, kNullIndex}) {
            auto builder = arrow::MakeBuilder(elem_type).ValueOrDie();
            for (int i = 0; i < kCount; ++i) {
                if (i == null_index) {
                    ASSERT_TRUE(builder->AppendNull().ok());
                } else {
                    ASSERT_TRUE(builder->AppendScalar(*gen(i)).ok());
                }
            }
            std::shared_ptr<arrow::Array> arr;
            ASSERT_TRUE(builder->Finish(&arr).ok());
            auto list_scalar = std::make_shared<arrow::ListScalar>(arr, arrow::list(elem_type));

            auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
            ASSERT_EQ(decoded.size(), 1u);
            EXPECT_TRUE(decoded[0]->Equals(*list_scalar)) << elem_type->ToString();
        }
    };

    check(arrow::boolean(),
          [](int i) { return std::make_shared<arrow::BooleanScalar>(i % 2 == 0); });
    check(arrow::int8(),
          [](int i) { return std::make_shared<arrow::Int8Scalar>(static_cast<int8_t>(i)); });
    check(arrow::int16(),
          [](int i) { return std::make_shared<arrow::Int16Scalar>(static_cast<int16_t>(i)); });
    check(arrow::int32(), [](int i) { return std::make_shared<arrow::Int32Scalar>(i); });
    check(arrow::int64(), [](int i) {
        return std::make_shared<arrow::Int64Scalar>(static_cast<int64_t>(i) * 1'000'000LL);
    });
    check(arrow::uint8(),
          [](int i) { return std::make_shared<arrow::UInt8Scalar>(static_cast<uint8_t>(i)); });
    check(arrow::uint16(),
          [](int i) { return std::make_shared<arrow::UInt16Scalar>(static_cast<uint16_t>(i)); });
    check(arrow::uint32(),
          [](int i) { return std::make_shared<arrow::UInt32Scalar>(static_cast<uint32_t>(i)); });
    check(arrow::uint64(),
          [](int i) { return std::make_shared<arrow::UInt64Scalar>(static_cast<uint64_t>(i)); });
    check(arrow::float16(),
          [](int i) { return std::make_shared<arrow::HalfFloatScalar>(static_cast<uint16_t>(i)); });
    check(arrow::float32(),
          [](int i) { return std::make_shared<arrow::FloatScalar>(static_cast<float>(i) * 0.5f); });
    check(arrow::float64(), [](int i) {
        return std::make_shared<arrow::DoubleScalar>(static_cast<double>(i) * 0.25);
    });
    check(arrow::date32(), [](int i) { return std::make_shared<arrow::Date32Scalar>(i); });
    check(arrow::date64(), [](int i) {
        return std::make_shared<arrow::Date64Scalar>(static_cast<int64_t>(i) * 86'400'000LL);
    });

    {
        auto type = arrow::time32(arrow::TimeUnit::MILLI);
        check(type, [type](int i) { return std::make_shared<arrow::Time32Scalar>(i, type); });
    }
    {
        auto type = arrow::time64(arrow::TimeUnit::MICRO);
        check(type, [type](int i) {
            return std::make_shared<arrow::Time64Scalar>(static_cast<int64_t>(i), type);
        });
    }
    {
        auto type = arrow::timestamp(arrow::TimeUnit::NANO);
        check(type, [type](int i) {
            return std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(i), type);
        });
    }
    {
        auto type = arrow::duration(arrow::TimeUnit::MILLI);
        check(type, [type](int i) {
            return std::make_shared<arrow::DurationScalar>(static_cast<int64_t>(i), type);
        });
    }
    check(arrow::month_interval(),
          [](int i) { return std::make_shared<arrow::MonthIntervalScalar>(i); });
    check(arrow::day_time_interval(), [](int i) {
        return std::make_shared<arrow::DayTimeIntervalScalar>(
            arrow::DayTimeIntervalType::DayMilliseconds{i, i * 2});
    });
    check(arrow::month_day_nano_interval(), [](int i) {
        return std::make_shared<arrow::MonthDayNanoIntervalScalar>(
            arrow::MonthDayNanoIntervalType::MonthDayNanos{i, i, static_cast<int64_t>(i) * 1000});
    });
    {
        auto type = arrow::decimal128(18, 4);
        check(type, [type](int i) {
            return std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(i * 12345), type);
        });
    }
    {
        auto type = arrow::decimal256(30, 6);
        check(type, [type](int i) {
            return std::make_shared<arrow::Decimal256Scalar>(
                arrow::Decimal256(static_cast<int64_t>(i) * 6789), type);
        });
    }
    {
        auto type = arrow::fixed_size_binary(6);
        check(type, [type](int i) {
            std::string s(6, static_cast<char>(0));
            for (int b = 0; b < 6; ++b)
                s[static_cast<size_t>(b)] = static_cast<char>((i + b) & 0xFF);
            return std::make_shared<arrow::FixedSizeBinaryScalar>(arrow::Buffer::FromString(s),
                                                                  type);
        });
    }
}

// ---------------------------------------------------------------------------
// Step 5a: Codec::EncodeRow rewritten on PositionalWriter + a typed-array
// walker (EncodeScalarValue/EncodeElement/EncodeElements in codec.cpp).
// ---------------------------------------------------------------------------

// The WriteBuffer-overload and the vector-returning overload must produce
// identical bytes (the latter is now just VectorWriteBuffer + Finish() over
// the former), and both must still match the hand-built PositionalWriter
// oracle for a representative spread of shapes already covered above.
TEST(CodecTest, EncodeIntoWriteBufferMatchesVectorOverload) {
    auto check_row = [](const std::shared_ptr<arrow::Schema>& schema,
                        const fletcher::ArrowRow& row) {
        fletcher::Codec codec(schema);
        auto vector_bytes = codec.EncodeRow(row);

        fletcher::VectorWriteBuffer buf;
        codec.EncodeRow(row, buf);
        auto direct_bytes = buf.Finish();

        EXPECT_EQ(vector_bytes, direct_bytes);
    };

    // Scalars + nulls.
    {
        auto schema = arrow::schema({
            arrow::field("a", arrow::int32(), true),
            arrow::field("b", arrow::utf8(), true),
            arrow::field("c", arrow::float64(), true),
        });
        check_row(schema, {arrow::MakeNullScalar(arrow::int32()),
                           std::make_shared<arrow::StringScalar>("hi"),
                           std::make_shared<arrow::DoubleScalar>(3.14)});
    }

    // A large fixed-width list (bulk path).
    {
        auto schema = arrow::schema({arrow::field("xs", arrow::list(arrow::float32()), true)});
        arrow::FloatBuilder fb;
        for (int i = 0; i < 2667; ++i) ASSERT_TRUE(fb.Append(static_cast<float>(i) * 0.5f).ok());
        auto arr = fb.Finish().ValueOrDie();
        check_row(schema, {std::make_shared<arrow::ListScalar>(arr, schema->field(0)->type())});
    }

    // Struct, fixed_size_list, map, union, dictionary — one row exercising all of them together.
    {
        auto stype = arrow::struct_(
            {arrow::field("x", arrow::int32(), true), arrow::field("y", arrow::utf8(), true)});
        auto fsl_type = arrow::fixed_size_list(arrow::float32(), 3);
        auto map_type = arrow::map(arrow::utf8(), arrow::int32());
        auto union_type = arrow::dense_union(
            {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
        auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
        auto schema = arrow::schema({
            arrow::field("s", stype, true),
            arrow::field("v", fsl_type, true),
            arrow::field("m", map_type, true),
            arrow::field("u", union_type, true),
            arrow::field("d", dict_type, true),
        });

        auto struct_scalar = std::make_shared<arrow::StructScalar>(
            arrow::ScalarVector{std::make_shared<arrow::Int32Scalar>(7),
                                std::make_shared<arrow::StringScalar>("hi")},
            stype);

        arrow::FloatBuilder fb;
        ASSERT_TRUE(fb.AppendValues({1.0f, 2.0f, 3.0f}).ok());
        auto fsl_scalar =
            std::make_shared<arrow::FixedSizeListScalar>(fb.Finish().ValueOrDie(), fsl_type);

        arrow::StringBuilder kb;
        arrow::Int32Builder vb;
        ASSERT_TRUE(kb.Append("a").ok());
        ASSERT_TRUE(vb.Append(1).ok());
        auto entries_type = arrow::struct_(
            {arrow::field("key", arrow::utf8(), false), arrow::field("value", arrow::int32())});
        auto entries = std::make_shared<arrow::StructArray>(
            entries_type, 1,
            arrow::ArrayVector{kb.Finish().ValueOrDie(), vb.Finish().ValueOrDie()});
        auto map_scalar = std::make_shared<arrow::MapScalar>(entries, map_type);

        auto union_scalar = std::make_shared<arrow::DenseUnionScalar>(
            std::make_shared<arrow::Int32Scalar>(42), /*type_code=*/0, union_type);

        auto dict_scalar = std::make_shared<arrow::StringScalar>("dict-value");

        check_row(schema, {struct_scalar, fsl_scalar, map_scalar, union_scalar, dict_scalar});
    }
}

// Every fixed-width type FixedWidth()/AppendRun() recognise (plus BOOL) must produce identical
// bytes whether the list value comes from a sliced array (forcing the bulk path to account for
// arr.offset()) or a fresh unsliced array of the same values, both with and without a null in the
// slice window (which forces the per-element path instead).
TEST(CodecTest, ListEncodeBulkPathMatchesElementPath) {
    constexpr int kTotal = 12;
    constexpr int64_t kSliceStart = 2;
    constexpr int64_t kSliceLen = 7;
    constexpr int kNullIndex = 5;  // absolute index into the 12-element array; inside [2, 9).

    auto check = [](const std::shared_ptr<arrow::DataType>& elem_type,
                    const std::function<std::shared_ptr<arrow::Scalar>(int)>& gen) {
        auto list_type = arrow::list(elem_type);
        auto schema = arrow::schema({arrow::field("v", list_type, true)});
        fletcher::Codec codec(schema);

        for (int null_index : {-1, kNullIndex}) {
            auto builder12 = arrow::MakeBuilder(elem_type).ValueOrDie();
            for (int i = 0; i < kTotal; ++i) {
                if (i == null_index) {
                    ASSERT_TRUE(builder12->AppendNull().ok());
                } else {
                    ASSERT_TRUE(builder12->AppendScalar(*gen(i)).ok());
                }
            }
            std::shared_ptr<arrow::Array> arr12;
            ASSERT_TRUE(builder12->Finish(&arr12).ok());
            auto sliced = arr12->Slice(kSliceStart, kSliceLen);

            auto builder7 = arrow::MakeBuilder(elem_type).ValueOrDie();
            for (int64_t j = 0; j < kSliceLen; ++j) {
                int i = static_cast<int>(kSliceStart + j);
                if (i == null_index) {
                    ASSERT_TRUE(builder7->AppendNull().ok());
                } else {
                    ASSERT_TRUE(builder7->AppendScalar(*gen(i)).ok());
                }
            }
            std::shared_ptr<arrow::Array> fresh7;
            ASSERT_TRUE(builder7->Finish(&fresh7).ok());

            auto scalar_sliced = std::make_shared<arrow::ListScalar>(sliced, list_type);
            auto scalar_fresh = std::make_shared<arrow::ListScalar>(fresh7, list_type);

            auto bytes_sliced = codec.EncodeRow({scalar_sliced});
            auto bytes_fresh = codec.EncodeRow({scalar_fresh});
            EXPECT_EQ(bytes_sliced, bytes_fresh)
                << elem_type->ToString() << " null_index=" << null_index;

            auto decoded = codec.DecodeRow(bytes_sliced);
            ASSERT_EQ(decoded.size(), 1u);
            EXPECT_TRUE(decoded[0]->Equals(*scalar_fresh)) << elem_type->ToString();
        }
    };

    check(arrow::boolean(),
          [](int i) { return std::make_shared<arrow::BooleanScalar>(i % 2 == 0); });
    check(arrow::int8(),
          [](int i) { return std::make_shared<arrow::Int8Scalar>(static_cast<int8_t>(i)); });
    check(arrow::int16(),
          [](int i) { return std::make_shared<arrow::Int16Scalar>(static_cast<int16_t>(i)); });
    check(arrow::int32(), [](int i) { return std::make_shared<arrow::Int32Scalar>(i); });
    check(arrow::int64(), [](int i) {
        return std::make_shared<arrow::Int64Scalar>(static_cast<int64_t>(i) * 1'000'000LL);
    });
    check(arrow::uint8(),
          [](int i) { return std::make_shared<arrow::UInt8Scalar>(static_cast<uint8_t>(i)); });
    check(arrow::uint16(),
          [](int i) { return std::make_shared<arrow::UInt16Scalar>(static_cast<uint16_t>(i)); });
    check(arrow::uint32(),
          [](int i) { return std::make_shared<arrow::UInt32Scalar>(static_cast<uint32_t>(i)); });
    check(arrow::uint64(),
          [](int i) { return std::make_shared<arrow::UInt64Scalar>(static_cast<uint64_t>(i)); });
    check(arrow::float16(),
          [](int i) { return std::make_shared<arrow::HalfFloatScalar>(static_cast<uint16_t>(i)); });
    check(arrow::float32(),
          [](int i) { return std::make_shared<arrow::FloatScalar>(static_cast<float>(i) * 0.5f); });
    check(arrow::float64(), [](int i) {
        return std::make_shared<arrow::DoubleScalar>(static_cast<double>(i) * 0.25);
    });
    check(arrow::date32(), [](int i) { return std::make_shared<arrow::Date32Scalar>(i); });
    check(arrow::date64(), [](int i) {
        return std::make_shared<arrow::Date64Scalar>(static_cast<int64_t>(i) * 86'400'000LL);
    });
    {
        auto type = arrow::time32(arrow::TimeUnit::MILLI);
        check(type, [type](int i) { return std::make_shared<arrow::Time32Scalar>(i, type); });
    }
    {
        auto type = arrow::time64(arrow::TimeUnit::MICRO);
        check(type, [type](int i) {
            return std::make_shared<arrow::Time64Scalar>(static_cast<int64_t>(i), type);
        });
    }
    {
        auto type = arrow::timestamp(arrow::TimeUnit::NANO);
        check(type, [type](int i) {
            return std::make_shared<arrow::TimestampScalar>(static_cast<int64_t>(i), type);
        });
    }
    {
        auto type = arrow::duration(arrow::TimeUnit::MILLI);
        check(type, [type](int i) {
            return std::make_shared<arrow::DurationScalar>(static_cast<int64_t>(i), type);
        });
    }
    check(arrow::month_interval(),
          [](int i) { return std::make_shared<arrow::MonthIntervalScalar>(i); });
    check(arrow::day_time_interval(), [](int i) {
        return std::make_shared<arrow::DayTimeIntervalScalar>(
            arrow::DayTimeIntervalType::DayMilliseconds{i, i * 2});
    });
    check(arrow::month_day_nano_interval(), [](int i) {
        return std::make_shared<arrow::MonthDayNanoIntervalScalar>(
            arrow::MonthDayNanoIntervalType::MonthDayNanos{i, i, static_cast<int64_t>(i) * 1000});
    });
    {
        auto type = arrow::decimal128(18, 4);
        check(type, [type](int i) {
            return std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(i * 12345), type);
        });
    }
    {
        auto type = arrow::decimal256(30, 6);
        check(type, [type](int i) {
            return std::make_shared<arrow::Decimal256Scalar>(
                arrow::Decimal256(static_cast<int64_t>(i) * 6789), type);
        });
    }
    {
        auto type = arrow::fixed_size_binary(6);
        check(type, [type](int i) {
            std::string s(6, static_cast<char>(0));
            for (int b = 0; b < 6; ++b)
                s[static_cast<size_t>(b)] = static_cast<char>((i + b) & 0xFF);
            return std::make_shared<arrow::FixedSizeBinaryScalar>(arrow::Buffer::FromString(s),
                                                                  type);
        });
    }
}

// list<struct<int32, utf8>> whose ListScalar value is a slice of a larger StructArray: the
// struct's own children must be indexed with the SAME absolute index as the struct itself
// (StructArray::field(k) is already offset-windowed), never re-sliced again.
TEST(CodecTest, StructListEncodeUsesWindowedChildren) {
    auto struct_type = arrow::struct_(
        {arrow::field("a", arrow::int32(), true), arrow::field("b", arrow::utf8(), true)});
    auto list_type = arrow::list(struct_type);
    auto schema = arrow::schema({arrow::field("v", list_type, true)});
    fletcher::Codec codec(schema);

    auto build = [&](int start, int count) {
        arrow::StructBuilder sb(
            struct_type, arrow::default_memory_pool(),
            std::vector<std::shared_ptr<arrow::ArrayBuilder>>{
                std::make_shared<arrow::Int32Builder>(), std::make_shared<arrow::StringBuilder>()});
        auto* ab = static_cast<arrow::Int32Builder*>(sb.field_builder(0));
        auto* bb = static_cast<arrow::StringBuilder*>(sb.field_builder(1));
        for (int i = start; i < start + count; ++i) {
            EXPECT_TRUE(sb.Append().ok());
            EXPECT_TRUE(ab->Append(i).ok());
            EXPECT_TRUE(bb->Append("s" + std::to_string(i)).ok());
        }
        std::shared_ptr<arrow::Array> arr;
        EXPECT_TRUE(sb.Finish(&arr).ok());
        return arr;
    };

    auto full = build(0, 8);
    auto sliced = full->Slice(3, 4);
    auto fresh = build(3, 4);

    auto scalar_sliced = std::make_shared<arrow::ListScalar>(sliced, list_type);
    auto scalar_fresh = std::make_shared<arrow::ListScalar>(fresh, list_type);

    auto bytes_sliced = codec.EncodeRow({scalar_sliced});
    auto bytes_fresh = codec.EncodeRow({scalar_fresh});
    EXPECT_EQ(bytes_sliced, bytes_fresh);

    auto decoded = codec.DecodeRow(bytes_sliced);
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*scalar_fresh));
}

// list<map<utf8,int32>> — exercises EncodeElement's MAP arm (a map nested inside a list, rather
// than a map as a row's top-level field, which goes through EncodeScalarValue's MAP arm instead).
TEST(CodecTest, MapElementEncodeMatchesScalarPath) {
    auto map_type = arrow::map(arrow::utf8(), arrow::int32());
    auto list_type = arrow::list(map_type);
    auto schema = arrow::schema({arrow::field("v", list_type, true)});
    fletcher::Codec codec(schema);

    // Two map elements: {"a":1,"b":2} and {"c":3}.
    arrow::StringBuilder kb;
    arrow::Int32Builder vb;
    ASSERT_TRUE(kb.Append("a").ok());
    ASSERT_TRUE(vb.Append(1).ok());
    ASSERT_TRUE(kb.Append("b").ok());
    ASSERT_TRUE(vb.Append(2).ok());
    ASSERT_TRUE(kb.Append("c").ok());
    ASSERT_TRUE(vb.Append(3).ok());
    auto keys = kb.Finish().ValueOrDie();
    auto vals = vb.Finish().ValueOrDie();

    arrow::Int32Builder offsets_b;
    ASSERT_TRUE(offsets_b.AppendValues(std::vector<int32_t>{0, 2, 3}).ok());
    auto offsets_arr = offsets_b.Finish().ValueOrDie();

    auto map_arr = arrow::MapArray::FromArrays(offsets_arr, keys, vals).ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(map_arr, list_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
}

// list<dense_union<int32,utf8>> and list<sparse_union<int32,utf8>> — exercises EncodeElement's
// union arm (DenseUnionArray::value_offset / UnionArray::field / child_id on array elements
// rather than a scalar's own type_code/value).
TEST(CodecTest, UnionInsideListRoundtrips) {
    // Dense union: [int(10), str("mid"), int(20)].
    {
        auto union_type = arrow::dense_union(
            {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
        auto list_type = arrow::list(union_type);
        auto schema = arrow::schema({arrow::field("v", list_type, true)});
        fletcher::Codec codec(schema);

        arrow::Int32Builder ib;
        ASSERT_TRUE(ib.AppendValues({10, 20}).ok());
        auto int_arr = ib.Finish().ValueOrDie();
        arrow::StringBuilder sb;
        ASSERT_TRUE(sb.Append("mid").ok());
        auto str_arr = sb.Finish().ValueOrDie();

        arrow::Int8Builder tb;
        ASSERT_TRUE(tb.AppendValues(std::vector<int8_t>{0, 1, 0}).ok());
        auto type_ids_arr = tb.Finish().ValueOrDie();
        arrow::Int32Builder ob;
        ASSERT_TRUE(ob.AppendValues(std::vector<int32_t>{0, 0, 1}).ok());
        auto offsets_arr = ob.Finish().ValueOrDie();

        // Explicit field names: Make()'s field_names default to {} (numeric names), which would
        // mismatch union_type's declared "i"/"s" and trip the ListScalar type-equality check.
        auto union_arr = arrow::DenseUnionArray::Make(*type_ids_arr, *offsets_arr,
                                                      {int_arr, str_arr}, {"i", "s"}, {0, 1})
                             .ValueOrDie();
        auto list_scalar = std::make_shared<arrow::ListScalar>(union_arr, list_type);

        auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
        ASSERT_EQ(decoded.size(), 1u);
        EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
    }

    // Sparse union: [int(10), str("mid"), int(20)].
    {
        auto union_type = arrow::sparse_union(
            {arrow::field("i", arrow::int32()), arrow::field("s", arrow::utf8())}, {0, 1});
        auto list_type = arrow::list(union_type);
        auto schema = arrow::schema({arrow::field("v", list_type, true)});
        fletcher::Codec codec(schema);

        arrow::Int32Builder ib;
        ASSERT_TRUE(ib.AppendValues({10, 0, 20}).ok());
        auto int_arr = ib.Finish().ValueOrDie();
        arrow::StringBuilder sb;
        ASSERT_TRUE(sb.Append("").ok());
        ASSERT_TRUE(sb.Append("mid").ok());
        ASSERT_TRUE(sb.Append("").ok());
        auto str_arr = sb.Finish().ValueOrDie();

        arrow::Int8Builder tb;
        ASSERT_TRUE(tb.AppendValues(std::vector<int8_t>{0, 1, 0}).ok());
        auto type_ids_arr = tb.Finish().ValueOrDie();

        // Explicit field names, for the same reason as the dense case above.
        auto union_arr =
            arrow::SparseUnionArray::Make(*type_ids_arr, {int_arr, str_arr}, {"i", "s"}, {0, 1})
                .ValueOrDie();
        auto list_scalar = std::make_shared<arrow::ListScalar>(union_arr, list_type);

        auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
        ASSERT_EQ(decoded.size(), 1u);
        EXPECT_TRUE(decoded[0]->Equals(*list_scalar));
    }
}

// list<dictionary<int32,utf8>> — exercises EncodeElement's DICTIONARY arm (GetValueIndex on an
// array element). Decoded elements are plain utf8 values: dictionary fields are always
// transferred as their resolved value, one value per row/element (see codec.hpp).
TEST(CodecTest, DictionaryInsideListRoundtrips) {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto list_type = arrow::list(dict_type);
    auto schema = arrow::schema({arrow::field("v", list_type, true)});
    fletcher::Codec codec(schema);

    arrow::StringBuilder vb;
    ASSERT_TRUE(vb.AppendValues({"x", "y", "z"}).ok());
    auto dict_values = vb.Finish().ValueOrDie();
    arrow::Int32Builder ib;
    ASSERT_TRUE(ib.AppendValues({2, 0, 1}).ok());
    auto indices = ib.Finish().ValueOrDie();
    auto dict_arr =
        arrow::DictionaryArray::FromArrays(dict_type, indices, dict_values).ValueOrDie();

    auto list_scalar = std::make_shared<arrow::ListScalar>(dict_arr, list_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    ASSERT_EQ(decoded.size(), 1u);

    arrow::StringBuilder expected_vb;
    ASSERT_TRUE(expected_vb.AppendValues({"z", "x", "y"}).ok());
    auto expected_scalar = std::make_shared<arrow::ListScalar>(expected_vb.Finish().ValueOrDie(),
                                                               arrow::list(arrow::utf8()));
    EXPECT_TRUE(decoded[0]->Equals(*expected_scalar));
}
