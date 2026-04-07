#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "row_codec.hpp"
#include "schema_evolution.hpp"

// Generated headers for the evolution test messages.
#include "evolution_v1.fletcher.pb.h"
#include "evolution_v2.fletcher.pb.h"

using namespace fletcher;

// ---------------------------------------------------------------------------
// Cross-version decode: V1 writer → V2 reader
// ---------------------------------------------------------------------------

TEST_CASE("Evolution: SensorV1 row decoded by SensorV2 reader") {
    // Encode with V1 schema.
    evolution::SensorV1ArrowRow v1;
    v1.set_id(42);
    v1.set_temperature(23.5f);
    v1.set_location("oslo");
    auto encoded = v1.Encode();

    // Decode with V2 codec.
    RowCodec v2_codec(evolution::SensorV2ArrowRowSchema());
    auto decoded = v2_codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 4);

    // id: same type (int32), no promotion needed.
    CHECK(decoded[0]->is_valid);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 42);

    // temperature: float → double promotion.
    CHECK(decoded[1]->is_valid);
    CHECK(decoded[1]->type->id() == arrow::Type::DOUBLE);
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value ==
          Catch::Approx(23.5).epsilon(1e-5));

    // location: same type (string), no promotion.
    CHECK(decoded[2]->is_valid);
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString() == "oslo");

    // label: not present in V1 → null.
    CHECK(!decoded[3]->is_valid);
}

// ---------------------------------------------------------------------------
// Cross-version decode: V2 writer → V1 reader (dropped column, narrowing)
// ---------------------------------------------------------------------------

TEST_CASE("Evolution: SensorV2 row decoded by SensorV1 reader throws (double→float illegal)") {
    // Encode with V2 schema.
    evolution::SensorV2ArrowRow v2;
    v2.set_id(7);
    v2.set_temperature(99.9);
    v2.set_location("bergen");
    v2.set_label("test");
    auto encoded = v2.Encode();

    // Decode with V1 codec — temperature is double in wire, float in reader.
    // double → float is not a legal Iceberg promotion.
    RowCodec v1_codec(evolution::SensorV1ArrowRowSchema());
    REQUIRE_THROWS_AS(v1_codec.DecodeRow(encoded), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Same-version roundtrip through generated classes
// ---------------------------------------------------------------------------

TEST_CASE("SensorV1 same-schema roundtrip") {
    evolution::SensorV1ArrowRow original;
    original.set_id(1);
    original.set_temperature(20.0f);
    original.set_location("trondheim");

    auto encoded = original.Encode();
    evolution::SensorV1ArrowRow restored(encoded);

    RowCodec codec(evolution::SensorV1ArrowRowSchema());
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 3);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 1);
    CHECK(static_cast<const arrow::FloatScalar&>(*decoded[1]).value == Catch::Approx(20.0f));
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString() == "trondheim");
}

TEST_CASE("SensorV2 same-schema roundtrip") {
    evolution::SensorV2ArrowRow original;
    original.set_id(2);
    original.set_temperature(18.5);
    original.set_location("stavanger");
    original.set_label("sensor-a");

    auto encoded = original.Encode();

    RowCodec codec(evolution::SensorV2ArrowRowSchema());
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 4);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 2);
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value == Catch::Approx(18.5));
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString() == "stavanger");
    CHECK(decoded[3]->is_valid);
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[3]).value->ToString() == "sensor-a");
}

TEST_CASE("SensorV2 with null optional field") {
    evolution::SensorV2ArrowRow original;
    original.set_id(3);
    original.set_temperature(15.0);
    original.set_location("tromso");
    // label not set → null

    auto encoded = original.Encode();

    RowCodec codec(evolution::SensorV2ArrowRowSchema());
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 4);
    CHECK(!decoded[3]->is_valid);
}
