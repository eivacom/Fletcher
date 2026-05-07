// Schema evolution scenarios: SensorV1 (id, float temperature, location)
// vs SensorV2 (same plus optional label, with temperature widened to double).
//
// Cross-version decoding is intentionally NOT supported — the positional
// wire format assumes both sides share the exact same schema. Schema
// discovery runs through the pub/sub topic mechanism. These tests instead
// verify that each version round-trips cleanly through both its generated
// class and arrow-bridge's Codec.

#include "evolution_v1.fletcher.pb.h"
#include "evolution_v2.fletcher.pb.h"

#include <arrow_bridge/codec.hpp>
#include <pubsub/owned_schema.hpp>

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <memory>

using namespace fletcher;

namespace {

// Convert a nanoarrow OwnedSchema (returned by generated SchemaXxx()) into
// an Apache Arrow C++ schema, for use with Codec.
std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

}  // namespace

// ── Same-version round-trips through Codec ────────────────────────────────

TEST(ProtoEvolutionTest, SensorV1SameSchemaRoundtrip) {
    fletcher_gen::evolution::SensorV1 original;
    original.set_id(1);
    original.set_temperature(20.0f);
    original.set_location("trondheim");

    auto encoded = original.Encode();
    auto schema  = ImportNano(fletcher_gen::evolution::SensorV1Schema());
    Codec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 3u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 1);
    EXPECT_FLOAT_EQ(static_cast<const arrow::FloatScalar&>(*decoded[1]).value, 20.0f);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString(), "trondheim");
}

TEST(ProtoEvolutionTest, SensorV2SameSchemaRoundtrip) {
    fletcher_gen::evolution::SensorV2 original;
    original.set_id(2);
    original.set_temperature(18.5);
    original.set_location("stavanger");
    original.set_label("sensor-a");

    auto encoded = original.Encode();
    auto schema  = ImportNano(fletcher_gen::evolution::SensorV2Schema());
    Codec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 4u);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*decoded[0]).value, 2);
    EXPECT_DOUBLE_EQ(static_cast<const arrow::DoubleScalar&>(*decoded[1]).value, 18.5);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[2]).value->ToString(), "stavanger");
    EXPECT_TRUE(decoded[3]->is_valid);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*decoded[3]).value->ToString(), "sensor-a");
}

TEST(ProtoEvolutionTest, SensorV2WithNullOptionalField) {
    fletcher_gen::evolution::SensorV2 original;
    original.set_id(3);
    original.set_temperature(15.0);
    original.set_location("tromso");
    // label not set → expect null

    auto encoded = original.Encode();
    auto schema  = ImportNano(fletcher_gen::evolution::SensorV2Schema());
    Codec codec(std::move(schema));
    auto decoded = codec.DecodeRow(encoded);

    ASSERT_EQ(decoded.size(), 4u);
    EXPECT_FALSE(decoded[3]->is_valid);
}

// ── Native round-trip through generated classes alone ────────────────────

TEST(ProtoEvolutionTest, SensorV1NativeRoundtrip) {
    fletcher_gen::evolution::SensorV1 original;
    original.set_id(42);
    original.set_temperature(25.5f);
    original.set_location("oslo");

    fletcher_gen::evolution::SensorV1 decoded(original.Encode());
    EXPECT_EQ(decoded.id(), 42);
    EXPECT_FLOAT_EQ(decoded.temperature(), 25.5f);
    EXPECT_EQ(decoded.location(), "oslo");
}

TEST(ProtoEvolutionTest, SensorV2NativeRoundtripWithOptional) {
    fletcher_gen::evolution::SensorV2 original;
    original.set_id(7);
    original.set_temperature(12.3);
    original.set_location("bergen");
    original.set_label("lbl-1");

    fletcher_gen::evolution::SensorV2 decoded(original.Encode());
    EXPECT_EQ(decoded.id(), 7);
    EXPECT_DOUBLE_EQ(decoded.temperature(), 12.3);
    EXPECT_EQ(decoded.location(), "bergen");
    ASSERT_TRUE(decoded.label().has_value());
    EXPECT_EQ(*decoded.label(), "lbl-1");
}
