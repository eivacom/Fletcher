#include <catch2/catch_test_macros.hpp>
#include <arrow/api.h>
#include <row_codec.hpp>

// Generated headers — produced by protoc-gen-arrow-row from each .proto file.
#include "simple.arrow_row.pb.h"
#include "temporal.arrow_row.pb.h"
#include "nested.arrow_row.pb.h"
#include "collections.arrow_row.pb.h"
#include "maps.arrow_row.pb.h"
#include "complex.arrow_row.pb.h"

// Helper: decode an encoded row using the same schema as the generated class.
template <typename GeneratedClass>
std::vector<std::shared_ptr<arrow::Scalar>> RoundTrip(const GeneratedClass& obj) {
    auto encoded = obj.Encode();
    arrow_row::RowCodec codec(GeneratedClass::ArrowSchema());
    return codec.DecodeRow(encoded);
}

// =============================================================================
// simple.proto — SensorReadingArrowRow
// =============================================================================

TEST_CASE("SensorReading: schema structure") {
    auto schema = integration::SensorReadingArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 10);

    // Non-optional scalars are not nullable.
    CHECK(schema->field(0)->name() == "sensor_id");
    CHECK(schema->field(0)->type()->id() == arrow::Type::INT32);
    CHECK_FALSE(schema->field(0)->nullable());

    CHECK(schema->field(1)->name() == "temperature");
    CHECK(schema->field(1)->type()->id() == arrow::Type::DOUBLE);
    CHECK_FALSE(schema->field(1)->nullable());

    CHECK(schema->field(3)->name() == "active");
    CHECK(schema->field(3)->type()->id() == arrow::Type::BOOL);

    CHECK(schema->field(4)->name() == "location");
    CHECK(schema->field(4)->type()->id() == arrow::Type::STRING);

    CHECK(schema->field(5)->name() == "payload");
    CHECK(schema->field(5)->type()->id() == arrow::Type::BINARY);

    // optional fields are nullable.
    CHECK(schema->field(8)->name() == "humidity");
    CHECK(schema->field(8)->type()->id() == arrow::Type::DOUBLE);
    CHECK(schema->field(8)->nullable());

    CHECK(schema->field(9)->name() == "label");
    CHECK(schema->field(9)->type()->id() == arrow::Type::STRING);
    CHECK(schema->field(9)->nullable());
}

TEST_CASE("SensorReading: encode is non-empty") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL);
    CHECK_FALSE(r.Encode().empty());
}

TEST_CASE("SensorReading: roundtrip — scalar values") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(42)
     .set_temperature(23.5)
     .set_pressure(1013.25f)
     .set_active(true)
     .set_location("Room 101")
     .set_payload("\xDE\xAD\xBE\xEF")
     .set_sequence(7u)
     .set_timestamp_ns(1'000'000'000LL);

    auto scalars = RoundTrip(r);
    REQUIRE(scalars.size() == 10);

    auto* id = dynamic_cast<arrow::Int32Scalar*>(scalars[0].get());
    REQUIRE(id != nullptr);
    CHECK(id->value == 42);

    auto* temp = dynamic_cast<arrow::DoubleScalar*>(scalars[1].get());
    REQUIRE(temp != nullptr);
    CHECK(temp->value == 23.5);

    auto* act = dynamic_cast<arrow::BooleanScalar*>(scalars[3].get());
    REQUIRE(act != nullptr);
    CHECK(act->value == true);

    auto* loc = dynamic_cast<arrow::StringScalar*>(scalars[4].get());
    REQUIRE(loc != nullptr);
    CHECK(loc->value->ToString() == "Room 101");

    auto* ts = dynamic_cast<arrow::Int64Scalar*>(scalars[7].get());
    REQUIRE(ts != nullptr);
    CHECK(ts->value == 1'000'000'000LL);
}

TEST_CASE("SensorReading: optional fields null when not set") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL);

    auto scalars = RoundTrip(r);
    CHECK_FALSE(scalars[8]->is_valid);  // humidity
    CHECK_FALSE(scalars[9]->is_valid);  // label
}

TEST_CASE("SensorReading: optional fields valid when set") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL).set_humidity(55.3).set_label("humid");

    auto scalars = RoundTrip(r);
    REQUIRE(scalars[8]->is_valid);
    REQUIRE(scalars[9]->is_valid);

    auto* hum = dynamic_cast<arrow::DoubleScalar*>(scalars[8].get());
    REQUIRE(hum != nullptr);
    CHECK(hum->value == 55.3);

    auto* lbl = dynamic_cast<arrow::StringScalar*>(scalars[9].get());
    REQUIRE(lbl != nullptr);
    CHECK(lbl->value->ToString() == "humid");
}

// =============================================================================
// temporal.proto — TimedEventArrowRow
// =============================================================================

TEST_CASE("TimedEvent: schema structure") {
    auto schema = integration::TimedEventArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 5);

    CHECK(schema->field(0)->name() == "event_id");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRING);
    CHECK_FALSE(schema->field(0)->nullable());

    // google.protobuf.Timestamp → timestamp(NANO)
    CHECK(schema->field(1)->name() == "occurred_at");
    CHECK(schema->field(1)->type()->id() == arrow::Type::TIMESTAMP);
    CHECK_FALSE(schema->field(1)->nullable());

    // google.protobuf.Duration → duration(NANO)
    CHECK(schema->field(2)->name() == "elapsed");
    CHECK(schema->field(2)->type()->id() == arrow::Type::DURATION);
    CHECK_FALSE(schema->field(2)->nullable());

    // DoubleValue wrapper → nullable double
    CHECK(schema->field(3)->name() == "score");
    CHECK(schema->field(3)->type()->id() == arrow::Type::DOUBLE);
    CHECK(schema->field(3)->nullable());

    // StringValue wrapper → nullable string
    CHECK(schema->field(4)->name() == "label");
    CHECK(schema->field(4)->type()->id() == arrow::Type::STRING);
    CHECK(schema->field(4)->nullable());
}

TEST_CASE("TimedEvent: roundtrip — WKT values") {
    integration::TimedEventArrowRow ev;
    ev.set_event_id("evt-001")
      .set_occurred_at(1'700'000'000'000'000'000LL)
      .set_elapsed(5'000'000'000LL)
      .set_score(9.5);
    // label intentionally left unset → null

    auto scalars = RoundTrip(ev);
    REQUIRE(scalars.size() == 5);

    auto* ts = dynamic_cast<arrow::TimestampScalar*>(scalars[1].get());
    REQUIRE(ts != nullptr);
    CHECK(ts->value == 1'700'000'000'000'000'000LL);

    auto* dur = dynamic_cast<arrow::DurationScalar*>(scalars[2].get());
    REQUIRE(dur != nullptr);
    CHECK(dur->value == 5'000'000'000LL);

    auto* sc = dynamic_cast<arrow::DoubleScalar*>(scalars[3].get());
    REQUIRE(sc != nullptr);
    CHECK(sc->is_valid);
    CHECK(sc->value == 9.5);

    // label not set → null scalar
    CHECK_FALSE(scalars[4]->is_valid);
}

// =============================================================================
// nested.proto — GeoPointArrowRow, AddressArrowRow, LocationArrowRow
// =============================================================================

TEST_CASE("GeoPoint: schema is flat scalars") {
    auto schema = integration::GeoPointArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 3);
    CHECK(schema->field(0)->name() == "latitude");
    CHECK(schema->field(0)->type()->id() == arrow::Type::DOUBLE);
    CHECK(schema->field(1)->name() == "longitude");
    CHECK(schema->field(2)->name() == "elevation");
    CHECK(schema->field(2)->type()->id() == arrow::Type::FLOAT);
}

TEST_CASE("Location: schema embeds struct fields") {
    auto schema = integration::LocationArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 3);

    CHECK(schema->field(0)->name() == "point");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRUCT);
    CHECK_FALSE(schema->field(0)->nullable());

    CHECK(schema->field(1)->name() == "address");
    CHECK(schema->field(1)->type()->id() == arrow::Type::STRUCT);

    CHECK(schema->field(2)->name() == "name");
    CHECK(schema->field(2)->type()->id() == arrow::Type::STRING);
}

TEST_CASE("Location: roundtrip — nested structs") {
    integration::GeoPointArrowRow gp;
    gp.set_latitude(37.7749).set_longitude(-122.4194).set_elevation(16.0f);

    integration::AddressArrowRow addr;
    addr.set_street("1 Market St").set_city("San Francisco").set_country("US");

    integration::LocationArrowRow loc;
    loc.set_point(gp).set_address(addr).set_name("HQ");

    auto scalars = RoundTrip(loc);
    REQUIRE(scalars.size() == 3);

    CHECK(scalars[0]->type->id() == arrow::Type::STRUCT);
    CHECK(scalars[0]->is_valid);

    CHECK(scalars[1]->type->id() == arrow::Type::STRUCT);
    CHECK(scalars[1]->is_valid);

    auto* name = dynamic_cast<arrow::StringScalar*>(scalars[2].get());
    REQUIRE(name != nullptr);
    CHECK(name->value->ToString() == "HQ");
}

// =============================================================================
// collections.proto — PlayerArrowRow, TeamArrowRow
// =============================================================================

TEST_CASE("Team: schema has list fields") {
    auto schema = integration::TeamArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 4);

    CHECK(schema->field(0)->name() == "name");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRING);

    CHECK(schema->field(1)->name() == "members");
    CHECK(schema->field(1)->type()->id() == arrow::Type::LIST);
    CHECK_FALSE(schema->field(1)->nullable());

    CHECK(schema->field(2)->name() == "scores");
    CHECK(schema->field(2)->type()->id() == arrow::Type::LIST);

    CHECK(schema->field(3)->name() == "roster");
    CHECK(schema->field(3)->type()->id() == arrow::Type::LIST);
}

TEST_CASE("Team: roundtrip — repeated scalars and structs") {
    integration::PlayerArrowRow p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    integration::TeamArrowRow team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob", "Carol"})
        .set_scores({95.0, 87.5, 92.0})
        .set_roster({p1, p2});

    auto scalars = RoundTrip(team);
    REQUIRE(scalars.size() == 4);

    auto* name = dynamic_cast<arrow::StringScalar*>(scalars[0].get());
    REQUIRE(name != nullptr);
    CHECK(name->value->ToString() == "Alpha");

    auto* members = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    REQUIRE(members != nullptr);
    CHECK(members->value->length() == 3);

    auto* scores = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    REQUIRE(scores != nullptr);
    CHECK(scores->value->length() == 3);

    auto* roster = dynamic_cast<arrow::ListScalar*>(scalars[3].get());
    REQUIRE(roster != nullptr);
    CHECK(roster->value->length() == 2);
}

TEST_CASE("Team: empty repeated fields produce empty lists") {
    integration::TeamArrowRow team;
    team.set_name("Empty");

    auto scalars = RoundTrip(team);
    REQUIRE(scalars.size() == 4);

    auto* members = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    REQUIRE(members != nullptr);
    CHECK(members->value->length() == 0);
}

// =============================================================================
// maps.proto — MetricsArrowRow
// =============================================================================

TEST_CASE("Metrics: schema has map fields") {
    auto schema = integration::MetricsArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 3);

    CHECK(schema->field(0)->name() == "resource_id");

    CHECK(schema->field(1)->name() == "gauges");
    CHECK(schema->field(1)->type()->id() == arrow::Type::MAP);
    CHECK_FALSE(schema->field(1)->nullable());

    CHECK(schema->field(2)->name() == "counters");
    CHECK(schema->field(2)->type()->id() == arrow::Type::MAP);
}

TEST_CASE("Metrics: roundtrip — map fields") {
    integration::MetricsArrowRow m;
    m.set_resource_id("srv-1")
     .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
     .set_counters({{"requests", INT64_C(10000)}, {"errors", INT64_C(3)}});

    auto scalars = RoundTrip(m);
    REQUIRE(scalars.size() == 3);

    CHECK(scalars[1]->type->id() == arrow::Type::MAP);
    CHECK(scalars[1]->is_valid);
    CHECK(scalars[2]->type->id() == arrow::Type::MAP);
}

// =============================================================================
// complex.proto — OrderItemArrowRow, OrderArrowRow
// =============================================================================

TEST_CASE("Order: schema combines WKT, list<struct>, map, and optional") {
    auto schema = integration::OrderArrowRow::ArrowSchema();
    REQUIRE(schema->num_fields() == 5);

    CHECK(schema->field(0)->name() == "order_id");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRING);
    CHECK_FALSE(schema->field(0)->nullable());

    CHECK(schema->field(1)->name() == "created_at");
    CHECK(schema->field(1)->type()->id() == arrow::Type::TIMESTAMP);
    CHECK_FALSE(schema->field(1)->nullable());

    CHECK(schema->field(2)->name() == "items");
    CHECK(schema->field(2)->type()->id() == arrow::Type::LIST);
    CHECK_FALSE(schema->field(2)->nullable());

    CHECK(schema->field(3)->name() == "tags");
    CHECK(schema->field(3)->type()->id() == arrow::Type::MAP);

    CHECK(schema->field(4)->name() == "customer_note");
    CHECK(schema->field(4)->type()->id() == arrow::Type::STRING);
    CHECK(schema->field(4)->nullable());
}

TEST_CASE("Order: roundtrip — full complex row") {
    integration::OrderItemArrowRow item1, item2;
    item1.set_product_id("SKU-001").set_quantity(2).set_unit_price(9.99);
    item2.set_product_id("SKU-002").set_quantity(1).set_unit_price(24.99)
         .set_note("gift wrap");

    integration::OrderArrowRow order;
    order.set_order_id("ORD-12345")
         .set_created_at(1'700'000'000'000'000'000LL)
         .set_items({item1, item2})
         .set_tags({{"priority", 1}, {"region", 3}})
         .set_customer_note("Leave at door");

    auto scalars = RoundTrip(order);
    REQUIRE(scalars.size() == 5);

    auto* oid = dynamic_cast<arrow::StringScalar*>(scalars[0].get());
    REQUIRE(oid != nullptr);
    CHECK(oid->value->ToString() == "ORD-12345");

    auto* ts = dynamic_cast<arrow::TimestampScalar*>(scalars[1].get());
    REQUIRE(ts != nullptr);
    CHECK(ts->value == 1'700'000'000'000'000'000LL);

    auto* items = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    REQUIRE(items != nullptr);
    CHECK(items->value->length() == 2);

    CHECK(scalars[3]->type->id() == arrow::Type::MAP);

    // customer_note was set → valid
    REQUIRE(scalars[4]->is_valid);
    auto* note = dynamic_cast<arrow::StringScalar*>(scalars[4].get());
    REQUIRE(note != nullptr);
    CHECK(note->value->ToString() == "Leave at door");
}

TEST_CASE("Order: customer_note null when not set") {
    integration::OrderArrowRow order;
    order.set_order_id("ORD-0").set_created_at(0LL);

    auto scalars = RoundTrip(order);
    REQUIRE(scalars.size() == 5);
    CHECK_FALSE(scalars[4]->is_valid);
}

TEST_CASE("OrderItem: optional note null then valid") {
    integration::OrderItemArrowRow item;
    item.set_product_id("SKU-X").set_quantity(1).set_unit_price(5.0);

    {
        auto scalars = RoundTrip(item);
        CHECK_FALSE(scalars[3]->is_valid);  // note not set
    }

    item.set_note("fragile");
    {
        auto scalars = RoundTrip(item);
        CHECK(scalars[3]->is_valid);
        auto* n = dynamic_cast<arrow::StringScalar*>(scalars[3].get());
        REQUIRE(n != nullptr);
        CHECK(n->value->ToString() == "fragile");
    }
}
