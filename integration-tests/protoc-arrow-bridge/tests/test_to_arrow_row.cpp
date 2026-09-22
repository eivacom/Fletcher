// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// ToArrowRow() / AppendTo() — the generated positional encoder (EncodeTo) is the
// oracle: for every field-kind combination in the fixture corpus, encoding a row
// through ToArrowRow() and then through the Codec must produce the exact same
// bytes as the message's own Encode(). AppendTo() is exercised both directly
// (filling a struct column) and indirectly (ToArrowRow's composite branches call
// it for every nested struct element).

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/arrow_bridge/detail/arrow_result.hpp>
#include <memory>
#include <vector>

#include "collections.fletcher.arrow.pb.h"
#include "collections.fletcher.pb.h"
#include "complex.fletcher.arrow.pb.h"
#include "complex.fletcher.pb.h"
#include "flatten.fletcher.arrow.pb.h"
#include "flatten.fletcher.pb.h"
#include "maps.fletcher.arrow.pb.h"
#include "maps.fletcher.pb.h"
#include "nested.fletcher.arrow.pb.h"
#include "nested.fletcher.pb.h"
#include "simple.fletcher.arrow.pb.h"
#include "simple.fletcher.pb.h"
#include "temporal.fletcher.arrow.pb.h"
#include "temporal.fletcher.pb.h"

using namespace fletcher;

// The generated positional encoder (msg.Encode()) and the Codec-over-ToArrowRow
// path must always agree byte-for-byte, for every message in the fixture corpus.
TEST(ToArrowRowTest, EncodesTheSameBytesAsEncodeTo) {
    // SensorReading: scalars, with the two optional fields both set...
    {
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
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::SensorReadingSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(r)), r.Encode());
    }
    // ...and both left null.
    {
        fletcher_gen::integration::SensorReading r;
        r.set_sensor_id(1)
            .set_temperature(0.0)
            .set_pressure(0.0f)
            .set_active(false)
            .set_location("")
            .set_payload("")
            .set_sequence(0u)
            .set_timestamp_ns(0LL);
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::SensorReadingSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(r)), r.Encode());
    }

    // Location: nested struct.
    {
        fletcher_gen::integration::GeoPoint gp;
        gp.set_latitude(37.7749).set_longitude(-122.4194).set_elevation(16.0f);
        fletcher_gen::integration::Address addr;
        addr.set_street("1 Market St").set_city("San Francisco").set_country("US");
        fletcher_gen::integration::Location loc;
        loc.set_point(gp).set_address(addr).set_name("HQ");
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::LocationSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(loc)), loc.Encode());
    }

    // Team: repeated scalar + repeated struct.
    {
        fletcher_gen::integration::Player p1, p2;
        p1.set_name("Alice").set_level(5);
        p2.set_name("Bob").set_level(3);
        fletcher_gen::integration::Team team;
        team.set_name("Alpha")
            .set_members({"Alice", "Bob", "Carol"})
            .set_scores({95.0, 87.5, 92.0})
            .set_roster({p1, p2});
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::TeamSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(team)), team.Encode());
    }

    // Metrics: maps.
    {
        fletcher_gen::integration::Metrics m;
        m.set_resource_id("srv-1")
            .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
            .set_counters({{"requests", INT64_C(10000)}, {"errors", INT64_C(3)}});
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::MetricsSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(m)), m.Encode());
    }

    // Order: WKT timestamp, repeated struct, map, optional.
    {
        fletcher_gen::integration::OrderItem item1, item2;
        item1.set_product_id("SKU-001").set_quantity(2).set_unit_price(9.99);
        item2.set_product_id("SKU-002").set_quantity(1).set_unit_price(24.99).set_note("gift wrap");
        fletcher_gen::integration::Order order;
        order.set_order_id("ORD-12345")
            .set_created_at(1'700'000'000'000'000'000LL)
            .set_items({item1, item2})
            .set_tags({{"priority", 1}, {"region", 3}})
            .set_customer_note("Leave at door");
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::OrderSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(order)), order.Encode());
    }

    // TimedEvent: timestamp + duration, nullable WKT wrappers.
    {
        fletcher_gen::integration::TimedEvent ev;
        ev.set_event_id("evt-001")
            .set_occurred_at(1'700'000'000'000'000'000LL)
            .set_elapsed(5'000'000'000LL)
            .set_score(9.5);
        // label left null.
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::TimedEventSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(ev)), ev.Encode());
    }

    // FlattenTestRow.shape: NESTED_LIST depth 2, produced by the chained
    // (fletcher.flatten) Polygon -> Ring -> Coord wrapper walk.
    {
        fletcher_gen::integration::Coord c1, c2, c3;
        c1.set_x(0.0).set_y(0.0);
        c2.set_x(1.0).set_y(0.0);
        c3.set_x(0.0).set_y(1.0);
        fletcher_gen::integration::FlattenTestRow r;
        r.set_reading(36.6f);
        r.set_position(fletcher_gen::integration::Point{});
        r.set_shape({{c1, c2, c3}});
        r.set_bad(fletcher_gen::integration::BadFlatten{});
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::FlattenTestRowSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(r)), r.Encode());
    }

    // Roster: a repeated enum, a map<string, Message>, and a depth-3 nested list — none of the
    // fixtures above exercise any of the three.
    {
        fletcher_gen::integration::Player captain;
        captain.set_name("Casey").set_level(9);

        fletcher_gen::integration::Card ace, king, queen, jack;
        ace.set_label("ace").set_rank(14);
        king.set_label("king").set_rank(13);
        queen.set_label("queen").set_rank(12);
        jack.set_label("jack").set_rank(11);

        fletcher_gen::integration::Roster roster;
        roster.set_captain(captain)
            .set_suits({static_cast<int32_t>(fletcher_gen::integration::Suit::SUIT_HEARTS),
                        static_cast<int32_t>(fletcher_gen::integration::Suit::SUIT_SPADES),
                        static_cast<int32_t>(fletcher_gen::integration::Suit::SUIT_CLUBS)})
            .set_by_name({{"ace", ace}, {"king", king}})
            .set_spreads({{{queen, jack}, {ace}}, {{king}}});
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::RosterSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(roster)), roster.Encode());
    }
}

TEST(ToArrowRowTest, WithEmptyCollections) {
    // Team with no members, no scores, no roster.
    {
        fletcher_gen::integration::Team team;
        team.set_name("Empty");
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::TeamSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(team)), team.Encode());
    }

    // Metrics with an empty map.
    {
        fletcher_gen::integration::Metrics m;
        m.set_resource_id("srv-empty");
        fletcher::Codec codec(fletcher_gen::integration::detail::ImportSchema(
            fletcher_gen::integration::MetricsSchema()));
        EXPECT_EQ(codec.EncodeRow(ToArrowRow(m)), m.Encode());
    }
}

TEST(ToArrowRowTest, TeamMembersReadableThroughView) {
    fletcher_gen::integration::Player p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    fletcher_gen::integration::Team team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob"})
        .set_scores({95.0, 87.5})
        .set_roster({p1, p2});

    fletcher_gen::integration::TeamView view(ToArrowRow(team));
    auto roster = view.roster();
    ASSERT_EQ(roster.size(), 2);
    EXPECT_EQ(roster[0].name(), "Alice");
    EXPECT_EQ(roster[0].level(), 5);
    EXPECT_EQ(roster[1].name(), "Bob");
    EXPECT_EQ(roster[1].level(), 3);
}

TEST(ToArrowRowTest, AppendToFillsAStructColumn) {
    fletcher_gen::integration::Player p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    fletcher_gen::integration::Team t0, t1, t2;
    t0.set_name("Alpha").set_members({"Alice"}).set_scores({1.0}).set_roster({p1});
    t1.set_name("Beta");  // no members, scores, or roster.
    t2.set_name("Gamma").set_members({"Bob"}).set_scores({2.0}).set_roster({p2});
    std::vector<fletcher_gen::integration::Team> teams = {t0, t1, t2};

    auto schema =
        fletcher_gen::integration::detail::ImportSchema(fletcher_gen::integration::TeamSchema());
    ASSERT_NE(schema, nullptr);
    auto builder = fletcher::detail::ValueOrThrow(
        arrow::MakeBuilder(arrow::struct_(schema->fields())), "test: MakeBuilder");
    auto& sb = static_cast<arrow::StructBuilder&>(*builder);

    for (const auto& t : teams) ASSERT_TRUE(AppendTo(sb, t).ok());

    auto arr = fletcher::detail::ValueOrThrow(sb.Finish(), "test: Finish");
    EXPECT_TRUE(arr->ValidateFull().ok());
    ASSERT_EQ(arr->length(), 3);

    // Every field, not just a sample (name): each column's value must match what the message's
    // own Encode() bytes decode to through Codec::DecodeRow — an oracle AppendTo never runs, so
    // this checks the struct-column path against an independent one, not against itself.
    const auto& struct_arr = static_cast<const arrow::StructArray&>(*arr);
    fletcher::Codec team_codec(schema);
    for (size_t row = 0; row < teams.size(); ++row) {
        auto expected = team_codec.DecodeRow(teams[row].Encode());
        ASSERT_EQ(expected.size(), static_cast<size_t>(struct_arr.num_fields()));
        for (int c = 0; c < struct_arr.num_fields(); ++c) {
            auto got = fletcher::detail::ValueOrThrow(
                struct_arr.field(c)->GetScalar(static_cast<int64_t>(row)), "test: GetScalar");
            EXPECT_TRUE(got->Equals(*expected[static_cast<size_t>(c)]))
                << "row " << row << " field " << c << ": got " << got->ToString() << ", want "
                << expected[static_cast<size_t>(c)]->ToString();
        }
    }

    // A message carrying struct + map + nested-list fields together: a direct AppendTo(Roster)
    // call dispatches EmitStruct (captain), EmitMap (by_name) and EmitNestedList (spreads, depth
    // 3) in the SAME call, so all three actually run at runtime rather than only being emitted.
    fletcher_gen::integration::Player captain0, captain1;
    captain0.set_name("Casey").set_level(9);
    captain1.set_name("Drew").set_level(4);

    fletcher_gen::integration::Card ace, king, queen, jack;
    ace.set_label("ace").set_rank(14);
    king.set_label("king").set_rank(13);
    queen.set_label("queen").set_rank(12);
    jack.set_label("jack").set_rank(11);

    fletcher_gen::integration::Roster r0, r1;
    r0.set_captain(captain0)
        .set_suits({static_cast<int32_t>(fletcher_gen::integration::Suit::SUIT_HEARTS),
                    static_cast<int32_t>(fletcher_gen::integration::Suit::SUIT_SPADES)})
        .set_by_name({{"ace", ace}, {"king", king}})
        .set_spreads({{{queen, jack}, {ace}}, {{king}}});
    r1.set_captain(captain1);  // empty suits, empty map, empty spreads
    std::vector<fletcher_gen::integration::Roster> rosters = {r0, r1};

    auto roster_schema =
        fletcher_gen::integration::detail::ImportSchema(fletcher_gen::integration::RosterSchema());
    ASSERT_NE(roster_schema, nullptr);
    auto roster_builder = fletcher::detail::ValueOrThrow(
        arrow::MakeBuilder(arrow::struct_(roster_schema->fields())), "test: MakeBuilder");
    auto& rb = static_cast<arrow::StructBuilder&>(*roster_builder);

    for (const auto& r : rosters) ASSERT_TRUE(AppendTo(rb, r).ok());

    auto roster_arr = fletcher::detail::ValueOrThrow(rb.Finish(), "test: Finish");
    EXPECT_TRUE(roster_arr->ValidateFull().ok());
    ASSERT_EQ(roster_arr->length(), 2);

    const auto& roster_struct_arr = static_cast<const arrow::StructArray&>(*roster_arr);
    fletcher::Codec roster_codec(roster_schema);
    for (size_t row = 0; row < rosters.size(); ++row) {
        auto expected = roster_codec.DecodeRow(rosters[row].Encode());
        ASSERT_EQ(expected.size(), static_cast<size_t>(roster_struct_arr.num_fields()));
        for (int c = 0; c < roster_struct_arr.num_fields(); ++c) {
            auto got = fletcher::detail::ValueOrThrow(
                roster_struct_arr.field(c)->GetScalar(static_cast<int64_t>(row)),
                "test: GetScalar");
            EXPECT_TRUE(got->Equals(*expected[static_cast<size_t>(c)]))
                << "row " << row << " field " << c << ": got " << got->ToString() << ", want "
                << expected[static_cast<size_t>(c)]->ToString();
        }
    }
}

// A6: AppendTo checks the struct builder's field count against the schema's
// before touching it, so a caller-supplied builder built for the wrong
// message (or a stale one, after a schema change) fails cleanly instead of
// writing a child value into the wrong slot.
TEST(ToArrowRowTest, AppendToRejectsAStructBuilderWithTheWrongFieldCount) {
    auto full_fields =
        fletcher_gen::integration::detail::ImportSchema(fletcher_gen::integration::PlayerSchema())
            ->fields();
    ASSERT_EQ(full_fields.size(), 2u);  // Player: name, level
    std::vector<std::shared_ptr<arrow::Field>> wrong_fields = {full_fields[0]};  // drop "level"

    auto builder = fletcher::detail::ValueOrThrow(arrow::MakeBuilder(arrow::struct_(wrong_fields)),
                                                  "test: MakeBuilder");
    auto& sb = static_cast<arrow::StructBuilder&>(*builder);

    fletcher_gen::integration::Player p;
    p.set_name("Alice").set_level(5);

    auto status = AppendTo(sb, p);
    EXPECT_TRUE(status.IsInvalid()) << status.ToString();
    EXPECT_EQ(sb.length(), 0);
}
