// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <fletcher/arrow_bridge/codec.hpp>

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
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Malformed-input / safety (Phase 1 hardening)
// ---------------------------------------------------------------------------

TEST(CodecTest, DecodeRejectsTrailingBytes) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    fletcher::Codec codec(schema);
    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(7)});
    row.push_back(0xAB);  // padding / corruption after a valid row
    EXPECT_THROW(codec.DecodeRow(row), std::invalid_argument);
}

TEST(CodecTest, DecodeRejectsTruncatedBuffer) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    fletcher::Codec codec(schema);
    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(7)});
    ASSERT_GT(row.size(), 1u);
    row.pop_back();  // one byte short
    EXPECT_THROW(codec.DecodeRow(row), std::invalid_argument);
}

TEST(CodecTest, DecodeRejectsOversizedListCount) {
    auto schema = arrow::schema({arrow::field("v", arrow::list(arrow::int32()))});
    fletcher::Codec codec(schema);
    // [row bitfield = 0x00 (field present)] [list COUNT = 0xFFFFFFFF] and nothing else.
    const std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
}

TEST(CodecTest, DecodeRejectsOversizedMapCount) {
    auto schema = arrow::schema({arrow::field("v", arrow::map(arrow::utf8(), arrow::int32()))});
    fletcher::Codec codec(schema);
    const std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), std::invalid_argument);
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
