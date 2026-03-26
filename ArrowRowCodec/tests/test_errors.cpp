#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"

// ---------------------------------------------------------------------------
// Schema hash mismatch
// ---------------------------------------------------------------------------

TEST_CASE("Decoding with a mismatched schema throws") {
    auto schema_a = arrow::schema({arrow::field("x", arrow::int32())});
    auto schema_b = arrow::schema({arrow::field("x", arrow::utf8())});  // different type

    arrow_row::RowCodec codec_a(schema_a);
    arrow_row::RowCodec codec_b(schema_b);

    auto row = codec_a.EncodeRow({std::make_shared<arrow::Int32Scalar>(42)});
    REQUIRE_THROWS_AS(codec_b.DecodeRow(row), std::invalid_argument);
}

TEST_CASE("Decoding with a schema of different field count throws") {
    auto schema_a = arrow::schema({arrow::field("x", arrow::int32())});
    auto schema_b = arrow::schema({arrow::field("x", arrow::int32()),
                                    arrow::field("y", arrow::int32())});

    arrow_row::RowCodec codec_a(schema_a);
    arrow_row::RowCodec codec_b(schema_b);

    auto row = codec_a.EncodeRow({std::make_shared<arrow::Int32Scalar>(1)});
    REQUIRE_THROWS_AS(codec_b.DecodeRow(row), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Truncated / malformed buffer
// ---------------------------------------------------------------------------

TEST_CASE("Truncated buffer throws on decode") {
    auto schema = arrow::schema({arrow::field("x", arrow::int32()),
                                  arrow::field("y", arrow::utf8())});
    arrow_row::RowCodec codec(schema);

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
    arrow_row::RowCodec codec(schema);

    arrow_row::ArrowRow empty;
    REQUIRE_THROWS(codec.DecodeRow(empty));
}

// ---------------------------------------------------------------------------
// Schema hash embedded in the row bytes
// ---------------------------------------------------------------------------

TEST_CASE("RowCodec embeds the schema hash in every encoded row") {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    arrow_row::RowCodec codec(schema);

    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(0)});

    // First 8 bytes are the schema hash (little-endian uint64).
    REQUIRE(row.size() >= 8u);
    uint64_t embedded = 0;
    std::memcpy(&embedded, row.data(), 8);
    CHECK(embedded == codec.schema_hash());
}

TEST_CASE("Version byte is 0x01") {
    auto schema = arrow::schema({arrow::field("v", arrow::int32())});
    arrow_row::RowCodec codec(schema);

    auto row = codec.EncodeRow({std::make_shared<arrow::Int32Scalar>(0)});

    // Byte 8 (after 8-byte hash) is the version.
    REQUIRE(row.size() >= 9u);
    CHECK(row[8] == 0x01u);
}

// ---------------------------------------------------------------------------
// DICTIONARY is explicitly unsupported
// ---------------------------------------------------------------------------

TEST_CASE("Encoding a DICTIONARY scalar throws std::invalid_argument") {
    auto dict_type = arrow::dictionary(arrow::int32(), arrow::utf8());
    auto schema    = arrow::schema({arrow::field("d", dict_type)});
    arrow_row::RowCodec codec(schema);

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
