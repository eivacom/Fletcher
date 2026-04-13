#include <gtest/gtest.h>

#include <arrow/api.h>

#include "row_codec.hpp"

// ---------------------------------------------------------------------------
// Schema hash mismatch with incompatible types
// ---------------------------------------------------------------------------

TEST(ErrorTest, DecodingWithIncompatibleTypeThrows) {
    auto schema_a = arrow::schema({
        arrow::field("x", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto schema_b = arrow::schema({
        arrow::field("x", arrow::utf8(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    fletcher::RowCodec codec_a(schema_a);
    fletcher::RowCodec codec_b(schema_b);

    auto row = codec_a.EncodeRow({std::make_shared<arrow::Int32Scalar>(42)});
    EXPECT_THROW(codec_b.DecodeRow(row), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Schema evolution: added column fills with null
// ---------------------------------------------------------------------------

TEST(ErrorTest, DecodingWithExtraReaderColumnFillsNull) {
    auto schema_a = arrow::schema({
        arrow::field("x", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto schema_b = arrow::schema({
        arrow::field("x", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"})),
        arrow::field("y", arrow::int32(), true,
                     arrow::key_value_metadata({"field_number"}, {"2"}))});

    fletcher::RowCodec codec_a(schema_a);
    fletcher::RowCodec codec_b(schema_b);

    auto row = codec_a.EncodeRow({std::make_shared<arrow::Int32Scalar>(1)});
    auto decoded = codec_b.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 2u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 1);
    EXPECT_FALSE(decoded[1]->is_valid);  // missing field -> null
}

// ---------------------------------------------------------------------------
// Schema evolution: dropped column is skipped
// ---------------------------------------------------------------------------

TEST(ErrorTest, DecodingWithFewerReaderColumnsSkipsUnknownFields) {
    auto schema_a = arrow::schema({
        arrow::field("x", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"})),
        arrow::field("y", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"2"}))});
    auto schema_b = arrow::schema({
        arrow::field("x", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    fletcher::RowCodec codec_a(schema_a);
    fletcher::RowCodec codec_b(schema_b);

    auto row = codec_a.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(10),
        std::make_shared<arrow::Int32Scalar>(20)});
    auto decoded = codec_b.DecodeRow(row);

    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 10);
}

// ---------------------------------------------------------------------------
// Truncated / malformed buffer
// ---------------------------------------------------------------------------

TEST(ErrorTest, TruncatedBufferThrowsOnDecode) {
    auto schema = arrow::schema({arrow::field("x", arrow::int32()),
                                  arrow::field("y", arrow::utf8())});
    fletcher::RowCodec codec(schema);

    auto row = codec.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("hello"),
    });

    // Chop off the back half.
    row.resize(row.size() / 2);
    EXPECT_ANY_THROW(codec.DecodeRow(row));
}

TEST(ErrorTest, EmptyBufferThrowsOnDecode) {
    auto schema = arrow::schema({arrow::field("x", arrow::int32())});
    fletcher::RowCodec codec(schema);

    fletcher::EncodedRow empty;
    EXPECT_ANY_THROW(codec.DecodeRow(empty));
}

// ---------------------------------------------------------------------------
// Schema hash embedded in the row bytes
// ---------------------------------------------------------------------------

TEST(ErrorTest, RowCodecEmbedsSchemaHashInEveryEncodedRow) {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    fletcher::RowCodec codec(schema);

    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(0)});

    // First 8 bytes are the schema hash (little-endian uint64).
    ASSERT_GE(row.size(), 8u);
    uint64_t embedded = 0;
    std::memcpy(&embedded, row.data(), 8);
    EXPECT_EQ(embedded, codec.schema_hash());
}

TEST(ErrorTest, FieldCountFollowsSchemaHashInWireFormat) {
    auto schema = arrow::schema({arrow::field("a", arrow::int32()),
                                  arrow::field("b", arrow::utf8())});
    fletcher::RowCodec codec(schema);

    auto row = codec.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(0),
        std::make_shared<arrow::StringScalar>("x"),
    });

    // Bytes 8-9 (after 8-byte hash) are the field count (uint16).
    ASSERT_GE(row.size(), 10u);
    uint16_t count = 0;
    std::memcpy(&count, row.data() + 8, 2);
    EXPECT_EQ(count, 2);
}

// ---------------------------------------------------------------------------
// DICTIONARY is explicitly unsupported
// ---------------------------------------------------------------------------

TEST(ErrorTest, EncodingDictionaryScalarThrowsInvalidArgument) {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema    = arrow::schema({arrow::field("d", dict_type)});
    fletcher::RowCodec codec(schema);

    // Build a minimal dictionary array and extract a DictionaryScalar from it.
    arrow::StringBuilder str_builder;
    ASSERT_TRUE(str_builder.Append("apple").ok());
    ASSERT_TRUE(str_builder.Append("banana").ok());
    auto dict_arr_result = str_builder.Finish();
    ASSERT_TRUE(dict_arr_result.ok());

    arrow::DictionaryScalar::ValueType val{
        std::make_shared<arrow::Int32Scalar>(0),  // index 0 -> "apple"
        *dict_arr_result,
    };
    auto scalar = std::make_shared<arrow::DictionaryScalar>(val, dict_type);

    EXPECT_THROW(codec.EncodeRow({scalar}), std::invalid_argument);
}
