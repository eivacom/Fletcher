#include <catch2/catch_all.hpp>

#include <arrow/api.h>

#include "positional_codec.hpp"

// Generated headers for the evolution test messages.
#include "evolution_v1.fletcher.pb.h"
#include "evolution_v2.fletcher.pb.h"

using namespace fletcher;

// ---------------------------------------------------------------------------
// Cross-version decode tests removed — the positional format assumes
// matching schemas.  Schema evolution is no longer supported at the wire
// level; schema discovery via the companion topic guarantees both sides
// share the same schema.
// ---------------------------------------------------------------------------

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

    PositionalCodec codec(evolution::SensorV1ArrowRowSchema());
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

    PositionalCodec codec(evolution::SensorV2ArrowRowSchema());
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

    PositionalCodec codec(evolution::SensorV2ArrowRowSchema());
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 4);
    CHECK(!decoded[3]->is_valid);
}
