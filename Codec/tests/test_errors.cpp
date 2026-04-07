#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"

// ---------------------------------------------------------------------------
// Schema hash mismatch with incompatible types
// ---------------------------------------------------------------------------

TEST_CASE("Decoding with an incompatible type throws (ILLEGAL promotion)") {
    auto schema_a = arrow::schema({
        arrow::field("x", arrow::int32(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});
    auto schema_b = arrow::schema({
        arrow::field("x", arrow::utf8(), false,
                     arrow::key_value_metadata({"field_number"}, {"1"}))});

    fletcher::RowCodec codec_a(schema_a);
    fletcher::RowCodec codec_b(schema_b);

    auto row = codec_a.EncodeRow({std::make_shared<arrow::Int32Scalar>(42)});
    REQUIRE_THROWS_AS(codec_b.DecodeRow(row), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Schema evolution: added column fills with null
// ---------------------------------------------------------------------------

TEST_CASE("Decoding with extra reader column fills null (evolution)") {
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

    REQUIRE(decoded.size() == 2);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 1);
    CHECK(!decoded[1]->is_valid);  // missing field → null
}

// ---------------------------------------------------------------------------
// Schema evolution: dropped column is skipped
// ---------------------------------------------------------------------------

TEST_CASE("Decoding with fewer reader columns skips unknown fields") {
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

    REQUIRE(decoded.size() == 1);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 10);
}

// ---------------------------------------------------------------------------
// Truncated / malformed buffer
// ---------------------------------------------------------------------------

TEST_CASE("Truncated buffer throws on decode") {
    auto schema = arrow::schema({arrow::field("x", arrow::int32()),
                                  arrow::field("y", arrow::utf8())});
    fletcher::RowCodec codec(schema);

    auto row = codec.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("hello"),
    });

    // Chop off the back half.
    row.resize(row.size() / 2);
    REQUIRE_THROWS(codec.DecodeRow(row));
}

TEST_CASE("Empty buffer throws on decode") {
    auto schema = arrow::schema({arrow::field("x", arrow::int32())});
    fletcher::RowCodec codec(schema);

    fletcher::EncodedRow empty;
    REQUIRE_THROWS(codec.DecodeRow(empty));
}

// ---------------------------------------------------------------------------
// Schema hash embedded in the row bytes
// ---------------------------------------------------------------------------

TEST_CASE("RowCodec embeds the schema hash in every encoded row") {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    fletcher::RowCodec codec(schema);

    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(0)});

    // First 8 bytes are the schema hash (little-endian uint64).
    REQUIRE(row.size() >= 8u);
    uint64_t embedded = 0;
    std::memcpy(&embedded, row.data(), 8);
    CHECK(embedded == codec.schema_hash());
}

TEST_CASE("Field count follows schema hash in wire format") {
    auto schema = arrow::schema({arrow::field("a", arrow::int32()),
                                  arrow::field("b", arrow::utf8())});
    fletcher::RowCodec codec(schema);

    auto row = codec.EncodeRow({
        std::make_shared<arrow::Int32Scalar>(0),
        std::make_shared<arrow::StringScalar>("x"),
    });

    // Bytes 8-9 (after 8-byte hash) are the field count (uint16).
    REQUIRE(row.size() >= 10u);
    uint16_t count = 0;
    std::memcpy(&count, row.data() + 8, 2);
    CHECK(count == 2);
}

// ---------------------------------------------------------------------------
// DICTIONARY is explicitly unsupported
// ---------------------------------------------------------------------------

TEST_CASE("Encoding a DICTIONARY scalar throws std::invalid_argument") {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema    = arrow::schema({arrow::field("d", dict_type)});
    fletcher::RowCodec codec(schema);

    // Build a minimal dictionary array and extract a DictionaryScalar from it.
    arrow::StringBuilder str_builder;
    REQUIRE(str_builder.Append("apple").ok());
    REQUIRE(str_builder.Append("banana").ok());
    auto dict_arr_result = str_builder.Finish();
    REQUIRE(dict_arr_result.ok());

    arrow::DictionaryScalar::ValueType val{
        std::make_shared<arrow::Int32Scalar>(0),  // index 0 → "apple"
        *dict_arr_result,
    };
    auto scalar = std::make_shared<arrow::DictionaryScalar>(val, dict_type);

    REQUIRE_THROWS_AS(codec.EncodeRow({scalar}), std::invalid_argument);
}
