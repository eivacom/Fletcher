// View-class tests — encode via generated row, decode via Codec to
// ArrowRow, then construct the generated *View class over the row and
// access fields through it.
//
// The View classes are emitted by protoc-gen-fletcher into
// <stem>.fletcher.arrow.pb.h headers and use ArrowRowView templates
// from arrow-bridge to give typed accessors over arrow::Scalar values.

#include "simple.fletcher.pb.h"
#include "simple.fletcher.arrow.pb.h"
#include "nested.fletcher.pb.h"
#include "nested.fletcher.arrow.pb.h"

#include <arrow_bridge/codec.hpp>
#include <pubsub/owned_schema.hpp>

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <memory>

using namespace fletcher;

namespace {

std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

ArrowRow RoundTrip(EncodedRow encoded, OwnedSchema nano_schema) {
    static EncodedRow kept_alive;
    kept_alive = std::move(encoded);
    auto schema = ImportNano(std::move(nano_schema));
    if (!schema) {
        ADD_FAILURE() << "RoundTrip: ImportNano failed";
        return {};
    }
    Codec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

}  // namespace

TEST(ViewTest, SensorReadingFromArrowRow) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(42)
     .set_temperature(23.5)
     .set_pressure(1013.25f)
     .set_active(true)
     .set_location("roof")
     .set_payload("\x01\x02\x03")
     .set_sequence(100)
     .set_timestamp_ns(1'700'000'000'000'000'000LL)
     .set_humidity(65.0)
     .set_label("test-label");

    auto row = RoundTrip(r.Encode(), fletcher_gen::integration::SensorReadingSchema());
    fletcher_gen::integration::SensorReadingView view(std::move(row));

    EXPECT_EQ(view.sensor_id(), 42);
    EXPECT_DOUBLE_EQ(view.temperature(), 23.5);
    EXPECT_FLOAT_EQ(view.pressure(), 1013.25f);
    EXPECT_EQ(view.active(), true);
    EXPECT_EQ(view.location(), "roof");
    EXPECT_EQ(view.payload(), std::string_view("\x01\x02\x03", 3));
    EXPECT_EQ(view.sequence(), 100);
    EXPECT_EQ(view.timestamp_ns(), 1'700'000'000'000'000'000LL);
    ASSERT_TRUE(view.humidity().has_value());
    EXPECT_DOUBLE_EQ(*view.humidity(), 65.0);
    ASSERT_TRUE(view.label().has_value());
    EXPECT_EQ(*view.label(), "test-label");
}

TEST(ViewTest, SensorReadingWithNulls) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(1)
     .set_temperature(0.0)
     .set_pressure(0.0f)
     .set_active(false)
     .set_location("")
     .set_payload("")
     .set_sequence(0)
     .set_timestamp_ns(0);
    // humidity and label left as nullopt

    auto row = RoundTrip(r.Encode(), fletcher_gen::integration::SensorReadingSchema());
    fletcher_gen::integration::SensorReadingView view(std::move(row));

    EXPECT_EQ(view.sensor_id(), 1);
    EXPECT_FALSE(view.humidity().has_value());
    EXPECT_FALSE(view.label().has_value());
}

TEST(ViewTest, NestedLocationFromArrowRow) {
    fletcher_gen::integration::GeoPoint pt;
    pt.set_latitude(55.6761).set_longitude(12.5683).set_elevation(10.0f);

    fletcher_gen::integration::Address addr;
    addr.set_street("Nyhavn 1").set_city("Copenhagen").set_country("Denmark");

    fletcher_gen::integration::Location loc;
    loc.set_name("Office").set_point(pt).set_address(addr);

    auto row = RoundTrip(loc.Encode(), fletcher_gen::integration::LocationSchema());
    fletcher_gen::integration::LocationView view(std::move(row));

    EXPECT_EQ(view.name(), "Office");
    auto pv = view.point();
    EXPECT_DOUBLE_EQ(pv.latitude(), 55.6761);
    EXPECT_DOUBLE_EQ(pv.longitude(), 12.5683);
    EXPECT_FLOAT_EQ(pv.elevation(), 10.0f);
    auto av = view.address();
    EXPECT_EQ(av.street(), "Nyhavn 1");
    EXPECT_EQ(av.city(), "Copenhagen");
    EXPECT_EQ(av.country(), "Denmark");
}
