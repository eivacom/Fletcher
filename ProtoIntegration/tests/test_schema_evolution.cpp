#include <catch2/catch_all.hpp>

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include "positional_codec.hpp"
#include <pubsub/owned_schema.hpp>

// Generated headers for the evolution test messages.
#include "evolution_v1.fletcher.pb.h"
#include "evolution_v2.fletcher.pb.h"

using namespace fletcher;

// Helper: import an OwnedSchema (nanoarrow) to shared_ptr<arrow::Schema>.
static std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    REQUIRE(result.ok());
    return *result;
}

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
    fletcher_gen::evolution::SensorV1 original;
    original.set_id(1);
    original.set_temperature(20.0f);
    original.set_location("trondheim");

    auto encoded = original.Encode();
    fletcher_gen::evolution::SensorV1 restored(encoded);

    auto schema = ImportNano(fletcher_gen::evolution::SensorV1Schema());
    PositionalCodec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 3);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 1);
    CHECK(static_cast<const arrow::FloatScalar&>(*decoded[1]).value == Catch::Approx(20.0f));
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString() == "trondheim");
}

TEST_CASE("SensorV2 same-schema roundtrip") {
    fletcher_gen::evolution::SensorV2 original;
    original.set_id(2);
    original.set_temperature(18.5);
    original.set_location("stavanger");
    original.set_label("sensor-a");

    auto encoded = original.Encode();

    auto schema = ImportNano(fletcher_gen::evolution::SensorV2Schema());
    PositionalCodec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 4);
    CHECK(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value == 2);
    CHECK(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value == Catch::Approx(18.5));
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString() == "stavanger");
    CHECK(decoded[3]->is_valid);
    CHECK(static_cast<const arrow::StringScalar&>(*decoded[3]).value->ToString() == "sensor-a");
}

TEST_CASE("SensorV2 with null optional field") {
    fletcher_gen::evolution::SensorV2 original;
    original.set_id(3);
    original.set_temperature(15.0);
    original.set_location("tromso");
    // label not set → null

    auto encoded = original.Encode();

    auto schema = ImportNano(fletcher_gen::evolution::SensorV2Schema());
    PositionalCodec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    REQUIRE(decoded.size() == 4);
    CHECK(!decoded[3]->is_valid);
}

// ---------------------------------------------------------------------------
// Native roundtrip (no Arrow dependency needed)
// ---------------------------------------------------------------------------

TEST_CASE("SensorV1 native roundtrip") {
    fletcher_gen::evolution::SensorV1 original;
    original.set_id(42);
    original.set_temperature(25.5f);
    original.set_location("oslo");

    fletcher_gen::evolution::SensorV1 decoded(original.Encode());
    CHECK(decoded.id() == 42);
    CHECK(decoded.temperature() == Catch::Approx(25.5f));
    CHECK(decoded.location() == "oslo");
}

TEST_CASE("SensorV2 native roundtrip with optional") {
    fletcher_gen::evolution::SensorV2 original;
    original.set_id(7);
    original.set_temperature(12.3);
    original.set_location("bergen");
    original.set_label("lbl-1");

    fletcher_gen::evolution::SensorV2 decoded(original.Encode());
    CHECK(decoded.id() == 7);
    CHECK(decoded.temperature() == Catch::Approx(12.3));
    CHECK(decoded.location() == "bergen");
    REQUIRE(decoded.label().has_value());
    CHECK(*decoded.label() == "lbl-1");
}
