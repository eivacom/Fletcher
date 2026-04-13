#include <gtest/gtest.h>

#include <arrow/api.h>

#include "row_codec.hpp"
#include "schema_evolution.hpp"

using namespace fletcher;

// ---------------------------------------------------------------------------
// WireTypeId conversion
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, ArrowTypeToWireTypeIdRoundTripsForScalarTypes) {
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::boolean()),  WireTypeId::BOOL);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::int8()),      WireTypeId::INT8);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::int16()),     WireTypeId::INT16);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::int32()),     WireTypeId::INT32);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::int64()),     WireTypeId::INT64);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::uint8()),     WireTypeId::UINT8);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::uint16()),    WireTypeId::UINT16);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::uint32()),    WireTypeId::UINT32);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::uint64()),    WireTypeId::UINT64);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::float32()),   WireTypeId::FLOAT32);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::float64()),   WireTypeId::FLOAT64);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::utf8()),      WireTypeId::STRING);
    EXPECT_EQ(ArrowTypeToWireTypeId(*arrow::binary()),    WireTypeId::BINARY);
}

// ---------------------------------------------------------------------------
// ClassifyPromotion
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, ClassifyPromotionIdentity) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT32, WireTypeId::INT32), PromotionKind::IDENTITY);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::STRING, WireTypeId::STRING), PromotionKind::IDENTITY);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::FLOAT64, WireTypeId::FLOAT64), PromotionKind::IDENTITY);
}

TEST(SchemaEvolutionTest, ClassifyPromotionSignedIntWidening) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT8, WireTypeId::INT16),  PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT8, WireTypeId::INT32),  PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT8, WireTypeId::INT64),  PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT16, WireTypeId::INT32), PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT16, WireTypeId::INT64), PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT32, WireTypeId::INT64), PromotionKind::WIDEN_INT);
}

TEST(SchemaEvolutionTest, ClassifyPromotionUnsignedIntWidening) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT8, WireTypeId::UINT16),  PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT8, WireTypeId::UINT32),  PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT8, WireTypeId::UINT64),  PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT16, WireTypeId::UINT32), PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT16, WireTypeId::UINT64), PromotionKind::WIDEN_INT);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT32, WireTypeId::UINT64), PromotionKind::WIDEN_INT);
}

TEST(SchemaEvolutionTest, ClassifyPromotionFloatWidening) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::FLOAT32, WireTypeId::FLOAT64), PromotionKind::WIDEN_FLOAT);
}

TEST(SchemaEvolutionTest, ClassifyPromotionInt32ToFloat64) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT32, WireTypeId::FLOAT64), PromotionKind::INT_TO_FLOAT);
}

TEST(SchemaEvolutionTest, ClassifyPromotionDecimal128ToDecimal256) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::DECIMAL128, WireTypeId::DECIMAL256), PromotionKind::WIDEN_INT);
}

TEST(SchemaEvolutionTest, ClassifyPromotionIllegal) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::STRING, WireTypeId::INT32),  PromotionKind::ILLEGAL);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::INT64, WireTypeId::INT32),   PromotionKind::ILLEGAL);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::FLOAT64, WireTypeId::INT32), PromotionKind::ILLEGAL);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::UINT32, WireTypeId::INT32),  PromotionKind::ILLEGAL);
}

TEST(SchemaEvolutionTest, ClassifyPromotionStringFamilyCompatible) {
    EXPECT_EQ(ClassifyPromotion(WireTypeId::STRING, WireTypeId::LARGE_STRING), PromotionKind::IDENTITY);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::LARGE_STRING, WireTypeId::STRING), PromotionKind::IDENTITY);
    EXPECT_EQ(ClassifyPromotion(WireTypeId::STRING, WireTypeId::STRING_VIEW),  PromotionKind::IDENTITY);
}

// ---------------------------------------------------------------------------
// Same-schema roundtrip with tagged format
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, SameSchemaRoundtripWithFieldNumberMetadata) {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"})),
        arrow::field("name", arrow::utf8(), true,
                     arrow::key_value_metadata({"field_number"}, {"2"})),
        arrow::field("score", arrow::float64(), false,
                     arrow::key_value_metadata({"field_number"}, {"3"})),
    });
    RowCodec codec(schema);

    ArrowRow in = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
        std::make_shared<arrow::DoubleScalar>(3.14),
    };

    auto row     = codec.EncodeRow(in);
    auto decoded = codec.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 42);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[1]).value->ToString(), "hello");
    EXPECT_DOUBLE_EQ(static_cast<const arrow::DoubleScalar&>(*decoded[2]).value, 3.14);
}

// ---------------------------------------------------------------------------
// Type promotion at decode time
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, TypePromotionInt32ToInt64) {
    auto writer_schema = arrow::schema({
        arrow::field("v", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto reader_schema = arrow::schema({
        arrow::field("v", arrow::int64(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    RowCodec writer(writer_schema);
    RowCodec reader(reader_schema);

    auto row     = writer.EncodeRow({std::make_shared<arrow::Int32Scalar>(42)});
    auto decoded = reader.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0]->type->id(), arrow::Type::INT64);
    EXPECT_EQ(static_cast<const arrow::Int64Scalar&>(*decoded[0]).value, 42);
}

TEST(SchemaEvolutionTest, TypePromotionFloatToDouble) {
    auto writer_schema = arrow::schema({
        arrow::field("v", arrow::float32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto reader_schema = arrow::schema({
        arrow::field("v", arrow::float64(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    RowCodec writer(writer_schema);
    RowCodec reader(reader_schema);

    auto row     = writer.EncodeRow({std::make_shared<arrow::FloatScalar>(2.5f)});
    auto decoded = reader.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0]->type->id(), arrow::Type::DOUBLE);
    EXPECT_DOUBLE_EQ(static_cast<const arrow::DoubleScalar&>(*decoded[0]).value, 2.5);
}

TEST(SchemaEvolutionTest, TypePromotionInt32ToDouble) {
    auto writer_schema = arrow::schema({
        arrow::field("v", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto reader_schema = arrow::schema({
        arrow::field("v", arrow::float64(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    RowCodec writer(writer_schema);
    RowCodec reader(reader_schema);

    auto row     = writer.EncodeRow({std::make_shared<arrow::Int32Scalar>(100)});
    auto decoded = reader.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded[0]->type->id(), arrow::Type::DOUBLE);
    EXPECT_DOUBLE_EQ(static_cast<const arrow::DoubleScalar&>(*decoded[0]).value, 100.0);
}

// ---------------------------------------------------------------------------
// DecodingMap cache
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, DecodingMapCachedAcrossMultipleDecodes) {
    auto writer_schema = arrow::schema({
        arrow::field("v", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto reader_schema = arrow::schema({
        arrow::field("v", arrow::int64(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    RowCodec writer(writer_schema);
    RowCodec reader(reader_schema);

    auto row1 = writer.EncodeRow({std::make_shared<arrow::Int32Scalar>(1)});
    auto row2 = writer.EncodeRow({std::make_shared<arrow::Int32Scalar>(2)});

    // Both decodes succeed -- the second reuses the cached map.
    auto d1 = reader.DecodeRow(row1);
    auto d2 = reader.DecodeRow(row2);

    EXPECT_EQ(static_cast<const arrow::Int64Scalar&>(*d1[0]).value, 1);
    EXPECT_EQ(static_cast<const arrow::Int64Scalar&>(*d2[0]).value, 2);
}

// ---------------------------------------------------------------------------
// Add / drop column evolution
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, AddedColumnInReaderFillsNull) {
    auto v1 = arrow::schema({
        arrow::field("a", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto v2 = arrow::schema({
        arrow::field("a", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"})),
        arrow::field("b", arrow::utf8(), true,
                     arrow::key_value_metadata({"field_number"}, {"2"}))});

    RowCodec writer(v1);
    RowCodec reader(v2);

    auto row     = writer.EncodeRow({std::make_shared<arrow::Int32Scalar>(7)});
    auto decoded = reader.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 7);
    EXPECT_FALSE(decoded[1]->is_valid);
}

TEST(SchemaEvolutionTest, DroppedColumnInReaderSilentlySkipped) {
    auto v1 = arrow::schema({
        arrow::field("a", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"})),
        arrow::field("b", arrow::utf8(), false,
                     arrow::key_value_metadata({"field_number"}, {"2"}))});
    auto v2 = arrow::schema({
        arrow::field("a", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    RowCodec writer(v1);
    RowCodec reader(v2);

    auto row = writer.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(5),
        std::make_shared<arrow::StringScalar>("gone")});
    auto decoded = reader.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 5);
}

// ---------------------------------------------------------------------------
// 10-field schema matching integration test pattern
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, TenFieldSchemaRoundtripSensorReadingPattern) {
    auto schema = arrow::schema({
        arrow::field("sensor_id", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"})),
        arrow::field("temperature", arrow::float64(), false,
                     arrow::key_value_metadata({"field_number"}, {"2"})),
        arrow::field("pressure", arrow::float32(), false,
                     arrow::key_value_metadata({"field_number"}, {"3"})),
        arrow::field("active", arrow::boolean(), false,
                     arrow::key_value_metadata({"field_number"}, {"4"})),
        arrow::field("location", arrow::utf8(), false,
                     arrow::key_value_metadata({"field_number"}, {"5"})),
        arrow::field("payload", arrow::binary(), false,
                     arrow::key_value_metadata({"field_number"}, {"6"})),
        arrow::field("sequence", arrow::uint32(), false,
                     arrow::key_value_metadata({"field_number"}, {"7"})),
        arrow::field("timestamp_ns", arrow::int64(), false,
                     arrow::key_value_metadata({"field_number"}, {"8"})),
        arrow::field("humidity", arrow::float64(), true,
                     arrow::key_value_metadata({"field_number"}, {"9"})),
        arrow::field("label", arrow::utf8(), true,
                     arrow::key_value_metadata({"field_number"}, {"10"})),
    });
    RowCodec codec(schema);

    ArrowRow in = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::DoubleScalar>(23.5),
        std::make_shared<arrow::FloatScalar>(1013.25f),
        std::make_shared<arrow::BooleanScalar>(true),
        std::make_shared<arrow::StringScalar>("Room 101"),
        std::make_shared<arrow::BinaryScalar>("DEAD"),
        std::make_shared<arrow::UInt32Scalar>(7u),
        std::make_shared<arrow::Int64Scalar>(1000000000LL),
        arrow::MakeNullScalar(arrow::float64()),
        arrow::MakeNullScalar(arrow::utf8()),
    };

    auto row = codec.EncodeRow(in);
    auto decoded = codec.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 10u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 42);
    EXPECT_DOUBLE_EQ(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value, 23.5);
    EXPECT_FALSE(decoded[8]->is_valid);
    EXPECT_FALSE(decoded[9]->is_valid);

    // Also test with a separate codec for decode (like integration tests do)
    RowCodec codec2(schema);
    auto decoded2 = codec2.DecodeRow(row);
    ASSERT_EQ(decoded2.size(), 10u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded2[0]).value, 42);
}

// ---------------------------------------------------------------------------
// Renamed column (same field_number, different field name)
// ---------------------------------------------------------------------------

TEST(SchemaEvolutionTest, RenamedColumnMatchedByFieldNumber) {
    auto v1 = arrow::schema({
        arrow::field("old_name", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto v2 = arrow::schema({
        arrow::field("new_name", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    RowCodec writer(v1);
    RowCodec reader(v2);

    auto row     = writer.EncodeRow({std::make_shared<arrow::Int32Scalar>(99)});
    auto decoded = reader.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 99);
}
