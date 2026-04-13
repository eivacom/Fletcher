#include <gtest/gtest.h>

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
    if (!result.ok()) { ADD_FAILURE() << "ImportSchema failed"; return nullptr; }
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

TEST(ProtoEvolutionTest, SensorV1SameSchemaRoundtrip) {
    evolution::SensorV1ArrowRow original;
    original.set_id(1);
    original.set_temperature(20.0f);
    original.set_location("trondheim");

    auto encoded = original.Encode();
    evolution::SensorV1ArrowRow restored(encoded);

    auto schema = ImportNano(evolution::SensorV1ArrowRowSchema());
    PositionalCodec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 3);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 1);
    EXPECT_FLOAT_EQ(static_cast<const arrow::FloatScalar&>(*decoded[1]).value, 20.0f);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString(), "trondheim");
}

TEST(ProtoEvolutionTest, SensorV2SameSchemaRoundtrip) {
    evolution::SensorV2ArrowRow original;
    original.set_id(2);
    original.set_temperature(18.5);
    original.set_location("stavanger");
    original.set_label("sensor-a");

    auto encoded = original.Encode();

    auto schema = ImportNano(evolution::SensorV2ArrowRowSchema());
    PositionalCodec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 4);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 2);
    EXPECT_DOUBLE_EQ(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value, 18.5);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString(), "stavanger");
    EXPECT_TRUE(decoded[3]->is_valid);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[3]).value->ToString(), "sensor-a");
}

TEST(ProtoEvolutionTest, SensorV2WithNullOptionalField) {
    evolution::SensorV2ArrowRow original;
    original.set_id(3);
    original.set_temperature(15.0);
    original.set_location("tromso");
    // label not set → null

    auto encoded = original.Encode();

    auto schema = ImportNano(evolution::SensorV2ArrowRowSchema());
    PositionalCodec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 4);
    EXPECT_FALSE(decoded[3]->is_valid);
}

// ---------------------------------------------------------------------------
// Native roundtrip (no Arrow dependency needed)
// ---------------------------------------------------------------------------

TEST(ProtoEvolutionTest, SensorV1NativeRoundtrip) {
    evolution::SensorV1ArrowRow original;
    original.set_id(42);
    original.set_temperature(25.5f);
    original.set_location("oslo");

    evolution::SensorV1ArrowRow decoded(original.Encode());
    EXPECT_EQ(decoded.id(), 42);
    EXPECT_FLOAT_EQ(decoded.temperature(), 25.5f);
    EXPECT_EQ(decoded.location(), "oslo");
}

TEST(ProtoEvolutionTest, SensorV2NativeRoundtripWithOptional) {
    evolution::SensorV2ArrowRow original;
    original.set_id(7);
    original.set_temperature(12.3);
    original.set_location("bergen");
    original.set_label("lbl-1");

    evolution::SensorV2ArrowRow decoded(original.Encode());
    EXPECT_EQ(decoded.id(), 7);
    EXPECT_DOUBLE_EQ(decoded.temperature(), 12.3);
    EXPECT_EQ(decoded.location(), "bergen");
    ASSERT_TRUE(decoded.label().has_value());
    EXPECT_EQ(*decoded.label(), "lbl-1");
}
