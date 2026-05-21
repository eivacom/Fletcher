// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Native round-trip tests — encode via generated class, decode via the
// generated class's decode constructor. No Apache Arrow C++ involved
// here; this verifies the generated typed code's encode/decode pair is
// internally consistent across the full set of proto types.
//
// Together with the Codec-side tests, this gives us:
//   - generated.Encode → generated.decode(bytes)  ← here (no Arrow)
//   - generated.Encode → Codec.DecodeRow(bytes)   ← in test_simple.cpp etc.

#include <gtest/gtest.h>

#include "collections.fletcher.pb.h"
#include "complex.fletcher.pb.h"
#include "maps.fletcher.pb.h"
#include "nested.fletcher.pb.h"
#include "simple.fletcher.pb.h"
#include "temporal.fletcher.pb.h"

TEST(NativeRoundtripTest, SimpleScalars) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(42)
        .set_temperature(23.5)
        .set_pressure(1013.25f)
        .set_active(true)
        .set_location("Room 101")
        .set_payload("\xDE\xAD\xBE\xEF")
        .set_sequence(7u)
        .set_timestamp_ns(1'000'000'000LL)
        .set_humidity(55.3)
        .set_label("humid");

    fletcher_gen::integration::SensorReading decoded(r.Encode());

    EXPECT_EQ(decoded.sensor_id(), 42);
    EXPECT_DOUBLE_EQ(decoded.temperature(), 23.5);
    EXPECT_FLOAT_EQ(decoded.pressure(), 1013.25f);
    EXPECT_EQ(decoded.active(), true);
    EXPECT_EQ(decoded.location(), "Room 101");
    EXPECT_EQ(decoded.payload(), "\xDE\xAD\xBE\xEF");
    EXPECT_EQ(decoded.sequence(), 7u);
    EXPECT_EQ(decoded.timestamp_ns(), 1'000'000'000LL);
    ASSERT_TRUE(decoded.humidity().has_value());
    EXPECT_DOUBLE_EQ(*decoded.humidity(), 55.3);
    ASSERT_TRUE(decoded.label().has_value());
    EXPECT_EQ(*decoded.label(), "humid");
}

TEST(NativeRoundtripTest, OptionalNulls) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(1)
        .set_temperature(0.0)
        .set_pressure(0.0f)
        .set_active(false)
        .set_location("")
        .set_payload("")
        .set_sequence(0u)
        .set_timestamp_ns(0LL);
    // humidity and label intentionally left null

    fletcher_gen::integration::SensorReading decoded(r.Encode());
    EXPECT_FALSE(decoded.humidity().has_value());
    EXPECT_FALSE(decoded.label().has_value());
}

TEST(NativeRoundtripTest, NestedStructs) {
    fletcher_gen::integration::GeoPoint gp;
    gp.set_latitude(37.7749).set_longitude(-122.4194).set_elevation(16.0f);

    fletcher_gen::integration::Address addr;
    addr.set_street("1 Market St").set_city("San Francisco").set_country("US");

    fletcher_gen::integration::Location loc;
    loc.set_point(gp).set_address(addr).set_name("HQ");

    fletcher_gen::integration::Location decoded(loc.Encode());
    EXPECT_DOUBLE_EQ(decoded.point().latitude(), 37.7749);
    EXPECT_DOUBLE_EQ(decoded.point().longitude(), -122.4194);
    EXPECT_FLOAT_EQ(decoded.point().elevation(), 16.0f);
    EXPECT_EQ(decoded.address().street(), "1 Market St");
    EXPECT_EQ(decoded.address().city(), "San Francisco");
    EXPECT_EQ(decoded.name(), "HQ");
}

TEST(NativeRoundtripTest, RepeatedScalarsAndStructs) {
    fletcher_gen::integration::Player p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    fletcher_gen::integration::Team team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob", "Carol"})
        .set_scores({95.0, 87.5, 92.0})
        .set_roster({p1, p2});

    fletcher_gen::integration::Team decoded(team.Encode());
    EXPECT_EQ(decoded.name(), "Alpha");
    ASSERT_EQ(decoded.members().size(), 3u);
    EXPECT_EQ(decoded.members()[0], "Alice");
    EXPECT_EQ(decoded.members()[1], "Bob");
    EXPECT_EQ(decoded.members()[2], "Carol");
    ASSERT_EQ(decoded.scores().size(), 3u);
    EXPECT_DOUBLE_EQ(decoded.scores()[0], 95.0);
    ASSERT_EQ(decoded.roster().size(), 2u);
    EXPECT_EQ(decoded.roster()[0].name(), "Alice");
    EXPECT_EQ(decoded.roster()[0].level(), 5);
    EXPECT_EQ(decoded.roster()[1].name(), "Bob");
}

TEST(NativeRoundtripTest, MapFields) {
    fletcher_gen::integration::Metrics m;
    m.set_resource_id("srv-1")
        .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
        .set_counters({{"requests", INT64_C(10000)}, {"errors", INT64_C(3)}});

    fletcher_gen::integration::Metrics decoded(m.Encode());
    EXPECT_EQ(decoded.resource_id(), "srv-1");
    ASSERT_EQ(decoded.gauges().size(), 2u);
    ASSERT_EQ(decoded.counters().size(), 2u);
}

TEST(NativeRoundtripTest, ComplexWktListStructMapAndOptional) {
    fletcher_gen::integration::OrderItem item1, item2;
    item1.set_product_id("SKU-001").set_quantity(2).set_unit_price(9.99);
    item2.set_product_id("SKU-002").set_quantity(1).set_unit_price(24.99).set_note("gift wrap");

    fletcher_gen::integration::Order order;
    order.set_order_id("ORD-12345")
        .set_created_at(1'700'000'000'000'000'000LL)
        .set_items({item1, item2})
        .set_tags({{"priority", 1}, {"region", 3}})
        .set_customer_note("Leave at door");

    fletcher_gen::integration::Order decoded(order.Encode());
    EXPECT_EQ(decoded.order_id(), "ORD-12345");
    EXPECT_EQ(decoded.created_at(), 1'700'000'000'000'000'000LL);
    ASSERT_EQ(decoded.items().size(), 2u);
    EXPECT_EQ(decoded.items()[0].product_id(), "SKU-001");
    EXPECT_EQ(decoded.items()[0].quantity(), 2);
    ASSERT_TRUE(decoded.items()[1].note().has_value());
    EXPECT_EQ(*decoded.items()[1].note(), "gift wrap");
    ASSERT_EQ(decoded.tags().size(), 2u);
    ASSERT_TRUE(decoded.customer_note().has_value());
    EXPECT_EQ(*decoded.customer_note(), "Leave at door");
}

TEST(NativeRoundtripTest, TemporalWkt) {
    fletcher_gen::integration::TimedEvent ev;
    ev.set_event_id("evt-001")
        .set_occurred_at(1'700'000'000'000'000'000LL)
        .set_elapsed(5'000'000'000LL)
        .set_score(9.5);

    fletcher_gen::integration::TimedEvent decoded(ev.Encode());
    EXPECT_EQ(decoded.event_id(), "evt-001");
    EXPECT_EQ(decoded.occurred_at(), 1'700'000'000'000'000'000LL);
    EXPECT_EQ(decoded.elapsed(), 5'000'000'000LL);
    ASSERT_TRUE(decoded.score().has_value());
    EXPECT_DOUBLE_EQ(*decoded.score(), 9.5);
    EXPECT_FALSE(decoded.label().has_value());
}
