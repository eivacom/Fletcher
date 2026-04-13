#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"
#include "schema_evolution.hpp"

using namespace fletcher;

// ---------------------------------------------------------------------------
// WireTypeId conversion
// ---------------------------------------------------------------------------

TEST_CASE("ArrowTypeToWireTypeId round-trips for scalar types") {
    CHECK(ArrowTypeToWireTypeId(*arrow::boolean())  == WireTypeId::BOOL);
    CHECK(ArrowTypeToWireTypeId(*arrow::int8())      == WireTypeId::INT8);
    CHECK(ArrowTypeToWireTypeId(*arrow::int16())     == WireTypeId::INT16);
    CHECK(ArrowTypeToWireTypeId(*arrow::int32())     == WireTypeId::INT32);
    CHECK(ArrowTypeToWireTypeId(*arrow::int64())     == WireTypeId::INT64);
    CHECK(ArrowTypeToWireTypeId(*arrow::uint8())     == WireTypeId::UINT8);
    CHECK(ArrowTypeToWireTypeId(*arrow::uint16())    == WireTypeId::UINT16);
    CHECK(ArrowTypeToWireTypeId(*arrow::uint32())    == WireTypeId::UINT32);
    CHECK(ArrowTypeToWireTypeId(*arrow::uint64())    == WireTypeId::UINT64);
    CHECK(ArrowTypeToWireTypeId(*arrow::float32())   == WireTypeId::FLOAT32);
    CHECK(ArrowTypeToWireTypeId(*arrow::float64())   == WireTypeId::FLOAT64);
    CHECK(ArrowTypeToWireTypeId(*arrow::utf8())      == WireTypeId::STRING);
    CHECK(ArrowTypeToWireTypeId(*arrow::binary())    == WireTypeId::BINARY);
}

// ---------------------------------------------------------------------------
// ClassifyPromotion
// ---------------------------------------------------------------------------

TEST_CASE("ClassifyPromotion: identity") {
    CHECK(ClassifyPromotion(WireTypeId::INT32, WireTypeId::INT32) == PromotionKind::IDENTITY);
    CHECK(ClassifyPromotion(WireTypeId::STRING, WireTypeId::STRING) == PromotionKind::IDENTITY);
    CHECK(ClassifyPromotion(WireTypeId::FLOAT64, WireTypeId::FLOAT64) == PromotionKind::IDENTITY);
}

TEST_CASE("ClassifyPromotion: signed int widening") {
    CHECK(ClassifyPromotion(WireTypeId::INT8, WireTypeId::INT16)  == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::INT8, WireTypeId::INT32)  == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::INT8, WireTypeId::INT64)  == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::INT16, WireTypeId::INT32) == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::INT16, WireTypeId::INT64) == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::INT32, WireTypeId::INT64) == PromotionKind::WIDEN_INT);
}

TEST_CASE("ClassifyPromotion: unsigned int widening") {
    CHECK(ClassifyPromotion(WireTypeId::UINT8, WireTypeId::UINT16)  == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::UINT8, WireTypeId::UINT32)  == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::UINT8, WireTypeId::UINT64)  == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::UINT16, WireTypeId::UINT32) == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::UINT16, WireTypeId::UINT64) == PromotionKind::WIDEN_INT);
    CHECK(ClassifyPromotion(WireTypeId::UINT32, WireTypeId::UINT64) == PromotionKind::WIDEN_INT);
}

TEST_CASE("ClassifyPromotion: float widening") {
    CHECK(ClassifyPromotion(WireTypeId::FLOAT32, WireTypeId::FLOAT64) == PromotionKind::WIDEN_FLOAT);
}

TEST_CASE("ClassifyPromotion: int32 to float64") {
    CHECK(ClassifyPromotion(WireTypeId::INT32, WireTypeId::FLOAT64) == PromotionKind::INT_TO_FLOAT);
}

TEST_CASE("ClassifyPromotion: decimal128 to decimal256") {
    CHECK(ClassifyPromotion(WireTypeId::DECIMAL128, WireTypeId::DECIMAL256) == PromotionKind::WIDEN_INT);
}

TEST_CASE("ClassifyPromotion: illegal promotions") {
    CHECK(ClassifyPromotion(WireTypeId::STRING, WireTypeId::INT32)  == PromotionKind::ILLEGAL);
    CHECK(ClassifyPromotion(WireTypeId::INT64, WireTypeId::INT32)   == PromotionKind::ILLEGAL);
    CHECK(ClassifyPromotion(WireTypeId::FLOAT64, WireTypeId::INT32) == PromotionKind::ILLEGAL);
    CHECK(ClassifyPromotion(WireTypeId::UINT32, WireTypeId::INT32)  == PromotionKind::ILLEGAL);
}

TEST_CASE("ClassifyPromotion: string family is compatible") {
    CHECK(ClassifyPromotion(WireTypeId::STRING, WireTypeId::LARGE_STRING) == PromotionKind::IDENTITY);
    CHECK(ClassifyPromotion(WireTypeId::LARGE_STRING, WireTypeId::STRING) == PromotionKind::IDENTITY);
    CHECK(ClassifyPromotion(WireTypeId::STRING, WireTypeId::STRING_VIEW)  == PromotionKind::IDENTITY);
}

// ---------------------------------------------------------------------------
// Same-schema roundtrip with tagged format
// ---------------------------------------------------------------------------

TEST_CASE("Same-schema roundtrip with field_number metadata") {
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

    REQUIRE(decoded.size() == 3);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 42);
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[1]).value->ToString() == "hello");
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[2]).value == Catch::Approx(3.14));
}

// ---------------------------------------------------------------------------
// Type promotion at decode time
// ---------------------------------------------------------------------------

TEST_CASE("Type promotion: int32 -> int64") {
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

    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->type->id() == arrow::Type::INT64);
    CHECK(static_cast<const arrow::Int64Scalar&>(*decoded[0]).value == 42);
}

TEST_CASE("Type promotion: float -> double") {
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

    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->type->id() == arrow::Type::DOUBLE);
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[0]).value == Catch::Approx(2.5));
}

TEST_CASE("Type promotion: int32 -> double") {
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

    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->type->id() == arrow::Type::DOUBLE);
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[0]).value == Catch::Approx(100.0));
}

// ---------------------------------------------------------------------------
// DecodingMap cache
// ---------------------------------------------------------------------------

TEST_CASE("DecodingMap is cached across multiple decodes") {
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

    // Both decodes succeed — the second reuses the cached map.
    auto d1 = reader.DecodeRow(row1);
    auto d2 = reader.DecodeRow(row2);

    CHECK(static_cast<const arrow::Int64Scalar&>(*d1[0]).value == 1);
    CHECK(static_cast<const arrow::Int64Scalar&>(*d2[0]).value == 2);
}

// ---------------------------------------------------------------------------
// Add / drop column evolution
// ---------------------------------------------------------------------------

TEST_CASE("Added column in reader fills null") {
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

    REQUIRE(decoded.size() == 2);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 7);
    CHECK(!decoded[1]->is_valid);
}

TEST_CASE("Dropped column in reader is silently skipped") {
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

    REQUIRE(decoded.size() == 1);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 5);
}

// ---------------------------------------------------------------------------
// 10-field schema matching integration test pattern
// ---------------------------------------------------------------------------

TEST_CASE("10-field schema roundtrip (SensorReading pattern)") {
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

    REQUIRE(decoded.size() == 10);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 42);
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value == 23.5);
    CHECK(!decoded[8]->is_valid);
    CHECK(!decoded[9]->is_valid);

    // Also test with a separate codec for decode (like integration tests do)
    RowCodec codec2(schema);
    auto decoded2 = codec2.DecodeRow(row);
    REQUIRE(decoded2.size() == 10);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded2[0]).value == 42);
}

// ---------------------------------------------------------------------------
// Renamed column (same field_number, different field name)
// ---------------------------------------------------------------------------

TEST_CASE("Renamed column matched by field_number") {
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

    REQUIRE(decoded.size() == 1);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 99);
}
