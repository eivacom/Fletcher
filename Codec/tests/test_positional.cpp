#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "positional_codec.hpp"

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<arrow::Scalar> Roundtrip(
    const std::shared_ptr<arrow::DataType>& type,
    const std::shared_ptr<arrow::Scalar>&   in)
{
    auto schema = arrow::schema({arrow::field("v", type, /*nullable=*/true)});
    fletcher::PositionalCodec codec(schema);
    auto row     = codec.EncodeRow({in});
    auto decoded = codec.DecodeRow(row);
    REQUIRE(decoded.size() == 1);
    return decoded[0];
}

#define CHECK_POS_RT(type_expr, scalar_expr)                    \
    do {                                                        \
        auto _orig = (scalar_expr);                             \
        auto _dec  = Roundtrip((type_expr), _orig);             \
        CHECK(_dec->Equals(*_orig));                            \
    } while (false)

}  // namespace

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

TEST_CASE("Positional: boolean roundtrip") {
    CHECK_POS_RT(arrow::boolean(), std::make_shared<arrow::BooleanScalar>(true));
    CHECK_POS_RT(arrow::boolean(), std::make_shared<arrow::BooleanScalar>(false));
}

TEST_CASE("Positional: integer roundtrip") {
    CHECK_POS_RT(arrow::int8(),  std::make_shared<arrow::Int8Scalar>(-42));
    CHECK_POS_RT(arrow::int16(), std::make_shared<arrow::Int16Scalar>(1234));
    CHECK_POS_RT(arrow::int32(), std::make_shared<arrow::Int32Scalar>(-1'000'000));
    CHECK_POS_RT(arrow::int64(), std::make_shared<arrow::Int64Scalar>(9'223'372'036'854'775'807LL));
    CHECK_POS_RT(arrow::uint8(),  std::make_shared<arrow::UInt8Scalar>(255u));
    CHECK_POS_RT(arrow::uint16(), std::make_shared<arrow::UInt16Scalar>(65535u));
    CHECK_POS_RT(arrow::uint32(), std::make_shared<arrow::UInt32Scalar>(4'294'967'295u));
    CHECK_POS_RT(arrow::uint64(), std::make_shared<arrow::UInt64Scalar>(18'446'744'073'709'551'615ull));
}

TEST_CASE("Positional: float roundtrip") {
    CHECK_POS_RT(arrow::float32(), std::make_shared<arrow::FloatScalar>(3.14f));
    CHECK_POS_RT(arrow::float64(), std::make_shared<arrow::DoubleScalar>(2.718281828));
    CHECK_POS_RT(arrow::float16(), std::make_shared<arrow::HalfFloatScalar>(0x3C00u));
}

TEST_CASE("Positional: string and binary roundtrip") {
    auto str = std::make_shared<arrow::StringScalar>("hello positional");
    CHECK_POS_RT(arrow::utf8(), str);

    auto bin_data = std::make_shared<arrow::Buffer>(
        reinterpret_cast<const uint8_t*>("\xDE\xAD\xBE\xEF"), 4);
    auto bin = std::make_shared<arrow::BinaryScalar>(bin_data);
    CHECK_POS_RT(arrow::binary(), bin);
}

TEST_CASE("Positional: temporal types roundtrip") {
    CHECK_POS_RT(arrow::date32(), std::make_shared<arrow::Date32Scalar>(19000));
    CHECK_POS_RT(arrow::date64(), std::make_shared<arrow::Date64Scalar>(1'640'995'200'000LL));
    auto ts_type = arrow::timestamp(arrow::TimeUnit::NANO);
    CHECK_POS_RT(ts_type, std::make_shared<arrow::TimestampScalar>(1'000'000'000LL, ts_type));
}

TEST_CASE("Positional: decimal roundtrip") {
    auto dec128_type = arrow::decimal128(10, 2);
    CHECK_POS_RT(dec128_type,
                 std::make_shared<arrow::Decimal128Scalar>(
                     arrow::Decimal128(12345), dec128_type));
}

// ---------------------------------------------------------------------------
// Nulls
// ---------------------------------------------------------------------------

TEST_CASE("Positional: null fields") {
    auto schema = arrow::schema({
        arrow::field("a", arrow::int32(), true),
        arrow::field("b", arrow::utf8(), true),
        arrow::field("c", arrow::float64(), true),
    });
    fletcher::PositionalCodec codec(schema);

    fletcher::ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(42),
        arrow::MakeNullScalar(arrow::utf8()),
        std::make_shared<arrow::DoubleScalar>(3.14),
    };

    auto encoded = codec.EncodeRow(row);
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 3);
    CHECK(decoded[0]->Equals(*row[0]));
    CHECK(!decoded[1]->is_valid);
    CHECK(decoded[2]->Equals(*row[2]));
}

TEST_CASE("Positional: all null fields") {
    auto schema = arrow::schema({
        arrow::field("a", arrow::int32(), true),
        arrow::field("b", arrow::utf8(), true),
    });
    fletcher::PositionalCodec codec(schema);

    fletcher::ArrowRow row = {
        arrow::MakeNullScalar(arrow::int32()),
        arrow::MakeNullScalar(arrow::utf8()),
    };

    auto encoded = codec.EncodeRow(row);
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 2);
    CHECK(!decoded[0]->is_valid);
    CHECK(!decoded[1]->is_valid);

    // All-null row should just be the bitfield: 1 byte with bits 0 and 1 set.
    REQUIRE(encoded.size() == 1);
    CHECK(encoded[0] == 0x03);
}

// ---------------------------------------------------------------------------
// Multi-field row
// ---------------------------------------------------------------------------

TEST_CASE("Positional: multi-field row roundtrip") {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32()),
        arrow::field("name", arrow::utf8()),
        arrow::field("active", arrow::boolean()),
        arrow::field("score", arrow::float64()),
    });
    fletcher::PositionalCodec codec(schema);

    fletcher::ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(100),
        std::make_shared<arrow::StringScalar>("alice"),
        std::make_shared<arrow::BooleanScalar>(true),
        std::make_shared<arrow::DoubleScalar>(99.5),
    };

    auto decoded = codec.DecodeRow(codec.EncodeRow(row));
    REQUIRE(decoded.size() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK(decoded[i]->Equals(*row[i]));
}

// ---------------------------------------------------------------------------
// Struct
// ---------------------------------------------------------------------------

TEST_CASE("Positional: struct roundtrip") {
    auto stype = arrow::struct_({
        arrow::field("x", arrow::int32()),
        arrow::field("y", arrow::utf8()),
    });
    auto schema = arrow::schema({arrow::field("s", stype)});
    fletcher::PositionalCodec codec(schema);

    auto struct_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{
            std::make_shared<arrow::Int32Scalar>(7),
            std::make_shared<arrow::StringScalar>("hi"),
        }, stype);

    auto decoded = codec.DecodeRow(codec.EncodeRow({struct_scalar}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->Equals(*struct_scalar));
}

TEST_CASE("Positional: struct with null child") {
    auto stype = arrow::struct_({
        arrow::field("x", arrow::int32(), true),
        arrow::field("y", arrow::utf8(), true),
    });
    auto schema = arrow::schema({arrow::field("s", stype)});
    fletcher::PositionalCodec codec(schema);

    auto struct_scalar = std::make_shared<arrow::StructScalar>(
        arrow::ScalarVector{
            arrow::MakeNullScalar(arrow::int32()),
            std::make_shared<arrow::StringScalar>("hello"),
        }, stype);

    auto decoded = codec.DecodeRow(codec.EncodeRow({struct_scalar}));
    REQUIRE(decoded.size() == 1);

    auto& ds = static_cast<const arrow::StructScalar&>(*decoded[0]);
    CHECK(!ds.value[0]->is_valid);
    CHECK(ds.value[1]->Equals(*std::make_shared<arrow::StringScalar>("hello")));
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

TEST_CASE("Positional: list roundtrip") {
    auto list_type = arrow::list(arrow::int32());
    auto schema = arrow::schema({arrow::field("nums", list_type)});
    fletcher::PositionalCodec codec(schema);

    auto builder = arrow::Int32Builder();
    REQUIRE(builder.AppendValues({10, 20, 30}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->Equals(*list_scalar));
}

TEST_CASE("Positional: list with nulls") {
    auto list_type = arrow::list(arrow::int32());
    auto schema = arrow::schema({arrow::field("nums", list_type)});
    fletcher::PositionalCodec codec(schema);

    auto builder = arrow::Int32Builder();
    REQUIRE(builder.Append(1).ok());
    REQUIRE(builder.AppendNull().ok());
    REQUIRE(builder.Append(3).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto list_scalar = std::make_shared<arrow::ListScalar>(arr);

    auto decoded = codec.DecodeRow(codec.EncodeRow({list_scalar}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->Equals(*list_scalar));
}

// ---------------------------------------------------------------------------
// Fixed-size list
// ---------------------------------------------------------------------------

TEST_CASE("Positional: fixed-size list roundtrip") {
    auto fsl_type = arrow::fixed_size_list(arrow::float32(), 3);
    auto schema = arrow::schema({arrow::field("vec", fsl_type)});
    fletcher::PositionalCodec codec(schema);

    auto builder = arrow::FloatBuilder();
    REQUIRE(builder.AppendValues({1.0f, 2.0f, 3.0f}).ok());
    auto arr = builder.Finish().ValueOrDie();
    auto fsl_scalar = std::make_shared<arrow::FixedSizeListScalar>(arr, fsl_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({fsl_scalar}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->Equals(*fsl_scalar));
}

// ---------------------------------------------------------------------------
// Map
// ---------------------------------------------------------------------------

TEST_CASE("Positional: map roundtrip") {
    auto map_type = arrow::map(arrow::utf8(), arrow::int32());
    auto schema = arrow::schema({arrow::field("m", map_type)});
    fletcher::PositionalCodec codec(schema);

    auto key_builder_ptr = arrow::StringBuilder();
    auto val_builder_ptr = arrow::Int32Builder();
    REQUIRE(key_builder_ptr.Append("a").ok());
    REQUIRE(val_builder_ptr.Append(1).ok());
    REQUIRE(key_builder_ptr.Append("b").ok());
    REQUIRE(val_builder_ptr.Append(2).ok());
    auto keys = key_builder_ptr.Finish().ValueOrDie();
    auto vals = val_builder_ptr.Finish().ValueOrDie();

    auto entries_type = arrow::struct_({
        arrow::field("key", arrow::utf8(), false),
        arrow::field("value", arrow::int32())});
    auto entries = std::make_shared<arrow::StructArray>(
        entries_type, 2, arrow::ArrayVector{keys, vals});
    auto map_scalar = std::make_shared<arrow::MapScalar>(entries);

    auto decoded = codec.DecodeRow(codec.EncodeRow({map_scalar}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->Equals(*map_scalar));
}

// ---------------------------------------------------------------------------
// Union
// ---------------------------------------------------------------------------

TEST_CASE("Positional: dense union roundtrip") {
    auto union_type = arrow::dense_union({
        arrow::field("i", arrow::int32()),
        arrow::field("s", arrow::utf8()),
    }, {0, 1});
    auto schema = arrow::schema({arrow::field("u", union_type)});
    fletcher::PositionalCodec codec(schema);

    // Encode an int variant.
    auto int_val = std::make_shared<arrow::Int32Scalar>(42);
    auto union_scalar = std::make_shared<arrow::DenseUnionScalar>(
        int_val, 0, union_type);

    auto decoded = codec.DecodeRow(codec.EncodeRow({union_scalar}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0]->Equals(*union_scalar));
}

// ---------------------------------------------------------------------------
// Wire size comparison: positional vs tagged
// ---------------------------------------------------------------------------

TEST_CASE("Positional: smaller wire size than tagged format") {
    auto schema = arrow::schema({
        arrow::field("id", arrow::int32()),
        arrow::field("x", arrow::float64()),
        arrow::field("y", arrow::float64()),
        arrow::field("z", arrow::float64()),
        arrow::field("name", arrow::utf8()),
        arrow::field("active", arrow::boolean()),
    });

    fletcher::PositionalCodec positional(schema);
    fletcher::ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::DoubleScalar>(1.0),
        std::make_shared<arrow::DoubleScalar>(2.0),
        std::make_shared<arrow::DoubleScalar>(3.0),
        std::make_shared<arrow::StringScalar>("sensor"),
        std::make_shared<arrow::BooleanScalar>(true),
    };

    auto positional_encoded = positional.EncodeRow(row);

    // Positional: 1 byte bitfield + 4 + 8 + 8 + 8 + (4+6) + 1 = 40 bytes
    // Tagged:     8 (hash) + 2 (count) + 6 * (4+1+1+4) = 70 bytes overhead
    //             + payloads = 70 + 4 + 8 + 8 + 8 + (4+6) + 1 = 109 bytes
    INFO("Positional size: " << positional_encoded.size());
    CHECK(positional_encoded.size() < 50);  // much smaller than ~109
}

// ---------------------------------------------------------------------------
// 9+ fields (multi-byte bitfield)
// ---------------------------------------------------------------------------

TEST_CASE("Positional: 9+ fields use multi-byte null bitfield") {
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
    fletcher::PositionalCodec codec(schema);

    auto decoded = codec.DecodeRow(codec.EncodeRow(row));
    REQUIRE(decoded.size() == 12);
    for (int i = 0; i < 12; ++i) {
        if (i % 3 == 0) {
            CHECK(!decoded[i]->is_valid);
        } else {
            CHECK(decoded[i]->Equals(*row[i]));
        }
    }
}
