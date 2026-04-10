#include <catch2/catch_test_macros.hpp>
#include <arrow/api.h>
#include <pubsub/pubsub_provider.hpp>
#include <positional_codec.hpp>

#include <map>

// Generated headers — produced by protoc-gen-fletcher from each .proto file.
#include "simple.fletcher.pb.h"
#include "temporal.fletcher.pb.h"
#include "nested.fletcher.pb.h"
#include "collections.fletcher.pb.h"
#include "maps.fletcher.pb.h"
#include "pubsub.fletcher.pb.h"
#include "complex.fletcher.pb.h"

// Helper: decode an encoded row using the given schema.
// The encoded data is kept alive in a static to prevent dangling Buffer
// pointers — DecodeScalarFromReader creates non-owning Buffers into the
// input data.
fletcher::ArrowRow RoundTrip(
    fletcher::EncodedRow encoded,
    std::shared_ptr<arrow::Schema> schema) {
    static fletcher::EncodedRow kept_alive;
    kept_alive = std::move(encoded);
    fletcher::PositionalCodec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

// =============================================================================
// simple.proto — SensorReadingArrowRow
// =============================================================================

TEST_CASE("SensorReading: schema structure") {
    auto schema = integration::SensorReadingArrowRowSchema();
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

    auto scalars = RoundTrip(r.Encode(), integration::SensorReadingArrowRowSchema());
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

    auto scalars = RoundTrip(r.Encode(), integration::SensorReadingArrowRowSchema());
    CHECK_FALSE(scalars[8]->is_valid);  // humidity
    CHECK_FALSE(scalars[9]->is_valid);  // label
}

TEST_CASE("SensorReading: optional fields valid when set") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL).set_humidity(55.3).set_label("humid");

    auto scalars = RoundTrip(r.Encode(), integration::SensorReadingArrowRowSchema());
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
    auto schema = integration::TimedEventArrowRowSchema();
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

    auto scalars = RoundTrip(ev.Encode(), integration::TimedEventArrowRowSchema());
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
    auto schema = integration::GeoPointArrowRowSchema();
    REQUIRE(schema->num_fields() == 3);
    CHECK(schema->field(0)->name() == "latitude");
    CHECK(schema->field(0)->type()->id() == arrow::Type::DOUBLE);
    CHECK(schema->field(1)->name() == "longitude");
    CHECK(schema->field(2)->name() == "elevation");
    CHECK(schema->field(2)->type()->id() == arrow::Type::FLOAT);
}

TEST_CASE("Location: schema embeds struct fields") {
    auto schema = integration::LocationArrowRowSchema();
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

    auto scalars = RoundTrip(loc.Encode(), integration::LocationArrowRowSchema());
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
    auto schema = integration::TeamArrowRowSchema();
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

    auto scalars = RoundTrip(team.Encode(), integration::TeamArrowRowSchema());
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

    auto scalars = RoundTrip(team.Encode(), integration::TeamArrowRowSchema());
    REQUIRE(scalars.size() == 4);

    auto* members = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    REQUIRE(members != nullptr);
    CHECK(members->value->length() == 0);
}

// =============================================================================
// maps.proto — MetricsArrowRow
// =============================================================================

TEST_CASE("Metrics: schema has map fields") {
    auto schema = integration::MetricsArrowRowSchema();
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

    auto scalars = RoundTrip(m.Encode(), integration::MetricsArrowRowSchema());
    REQUIRE(scalars.size() == 3);

    CHECK(scalars[1]->type->id() == arrow::Type::MAP);
    CHECK(scalars[1]->is_valid);
    CHECK(scalars[2]->type->id() == arrow::Type::MAP);
}

// =============================================================================
// complex.proto — OrderItemArrowRow, OrderArrowRow
// =============================================================================

TEST_CASE("Order: schema combines WKT, list<struct>, map, and optional") {
    auto schema = integration::OrderArrowRowSchema();
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

    auto scalars = RoundTrip(order.Encode(), integration::OrderArrowRowSchema());
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

    auto scalars = RoundTrip(order.Encode(), integration::OrderArrowRowSchema());
    REQUIRE(scalars.size() == 5);
    CHECK_FALSE(scalars[4]->is_valid);
}

TEST_CASE("OrderItem: optional note null then valid") {
    integration::OrderItemArrowRow item;
    item.set_product_id("SKU-X").set_quantity(1).set_unit_price(5.0);

    {
        auto scalars = RoundTrip(item.Encode(), integration::OrderItemArrowRowSchema());
        CHECK_FALSE(scalars[3]->is_valid);  // note not set
    }

    item.set_note("fragile");
    {
        auto scalars = RoundTrip(item.Encode(), integration::OrderItemArrowRowSchema());
        CHECK(scalars[3]->is_valid);
        auto* n = dynamic_cast<arrow::StringScalar*>(scalars[3].get());
        REQUIRE(n != nullptr);
        CHECK(n->value->ToString() == "fragile");
    }
}

// =============================================================================
// pubsub.proto — Publisher and Subscriber classes
// =============================================================================

namespace {

// Minimal mock that records calls and delivers published rows to subscribers.
class MockPubSubProvider : public fletcher::PubSubProvider {
 public:
    struct CreatedTopic {
        std::vector<std::string> segments;
        std::shared_ptr<arrow::Schema> schema;
    };

    struct PublishedMsg {
        std::vector<std::string> segments;
        fletcher::ArrowRow row;
        fletcher::Attachments attachments;
    };

    std::vector<CreatedTopic> created_topics;
    std::vector<PublishedMsg> published;
    std::map<std::vector<std::string>, SubscribeCallback> subscribers;

    void CreateTopic(const std::vector<std::string>& segments,
                     std::shared_ptr<arrow::Schema> schema) override {
        created_topics.push_back({segments, schema});
    }

    void Publish(const std::vector<std::string>& segments,
                 const fletcher::ArrowRow& row,
                 const fletcher::Attachments& attachments) override {
        published.push_back({segments, row, attachments});
        auto it = subscribers.find(segments);
        if (it != subscribers.end())
            it->second(row, attachments);
    }

    fletcher::SubscriptionResult Subscribe(
        const std::vector<std::string>& segments,
        SubscribeCallback callback) override {
        subscribers[segments] = std::move(callback);
        return {};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        subscribers.erase(segments);
    }
};

}  // namespace

// ---- Publisher tests -----------------------------------------------------

TEST_CASE("Publisher: construction creates topic with correct schema") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    REQUIRE(mock->created_topics.size() == 1);
    CHECK(mock->created_topics[0].segments ==
          std::vector<std::string>{"integration", "TelemetryFeed", "TelemetryStream"});
    CHECK(mock->created_topics[0].schema->num_fields() == 4);
}

TEST_CASE("Publisher: publish encodes and delivers to provider") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    integration::TelemetryArrowRow row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    REQUIRE(mock->published.size() == 1);
    CHECK(mock->published[0].segments ==
          std::vector<std::string>{"integration", "TelemetryFeed", "TelemetryStream"});
    CHECK_FALSE(mock->published[0].row.empty());
}

TEST_CASE("Publisher: multiple publishes accumulate") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    integration::TelemetryArrowRow r1, r2, r3;
    r1.set_device_id(1).set_value(1.0).set_timestamp(100LL).set_metric_name("a");
    r2.set_device_id(2).set_value(2.0).set_timestamp(200LL).set_metric_name("b");
    r3.set_device_id(3).set_value(3.0).set_timestamp(300LL).set_metric_name("c");

    pub.Publish(r1);
    pub.Publish(r2);
    pub.Publish(r3);

    CHECK(mock->published.size() == 3);
}

// ---- Subscriber tests ----------------------------------------------------

TEST_CASE("Subscriber: construction does not create topic") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    // Subscriber-only processes no longer call CreateTopic; the schema
    // is discovered from the provider when Subscribe() is called.
    CHECK(mock->created_topics.empty());
}

TEST_CASE("Subscriber: receives ArrowRow from published rows") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher::ArrowRow received;
    sub.Subscribe([&](fletcher::ArrowRow row, fletcher::Attachments) {
        received = std::move(row);
    });

    integration::TelemetryArrowRow row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    REQUIRE(received.size() == 4);

    auto* id = dynamic_cast<arrow::Int32Scalar*>(received[0].get());
    REQUIRE(id != nullptr);
    CHECK(id->value == 42);

    auto* name = dynamic_cast<arrow::StringScalar*>(received[3].get());
    REQUIRE(name != nullptr);
    CHECK(name->value->ToString() == "cpu");
}

TEST_CASE("Subscriber: unsubscribe stops delivery") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    int count = 0;
    sub.Subscribe([&](fletcher::ArrowRow, fletcher::Attachments) { ++count; });

    integration::TelemetryArrowRow row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");

    pub.Publish(row);
    CHECK(count == 1);

    sub.Unsubscribe();
    pub.Publish(row);
    CHECK(count == 1);  // no delivery after unsubscribe
}

TEST_CASE("Publisher: publish with attachments delivers blob to subscriber") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher::ArrowRow received_row;
    fletcher::Attachments received_att;
    sub.Subscribe([&](fletcher::ArrowRow row, fletcher::Attachments att) {
        received_row = std::move(row);
        received_att = std::move(att);
    });

    integration::TelemetryArrowRow row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    pub.Publish(row, {{"image", blob}});

    REQUIRE(received_row.size() == 4);
    REQUIRE(received_att.size() == 1);
    REQUIRE(received_att.count("image") == 1);
    CHECK(*received_att.at("image") == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});

    auto* id = dynamic_cast<arrow::Int32Scalar*>(received_row[0].get());
    REQUIRE(id != nullptr);
    CHECK(id->value == 42);
}

TEST_CASE("Publisher: publish without attachments has empty attachments") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    integration::TelemetryArrowRow row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");
    pub.Publish(row);

    REQUIRE(mock->published.size() == 1);
    CHECK(mock->published[0].attachments.empty());
    CHECK_FALSE(mock->published[0].row.empty());
}

// =============================================================================
// Byte-identical verification: EncodeTo vs ToScalars→RowCodec
//
// These tests verify that the generated EncodeTo(WriteBuffer&) produces
// identical wire bytes to the existing ToScalars() → RowCodec::EncodeRow path.
// =============================================================================

TEST_CASE("EncodeTo: simple scalars — byte identical to RowCodec path") {
    integration::SensorReadingArrowRow r;
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

    // Path A: EncodeTo (direct from C++ members)
    auto encoded_direct = r.Encode();

    // Path B: ToScalars → RowCodec::EncodeRow
    fletcher::RowCodec codec(integration::SensorReadingArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(r.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}

TEST_CASE("EncodeTo: optional nulls — byte identical") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL);
    // humidity and label intentionally left null

    auto encoded_direct = r.Encode();
    fletcher::RowCodec codec(integration::SensorReadingArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(r.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}

TEST_CASE("EncodeTo: nested structs — byte identical") {
    integration::GeoPointArrowRow gp;
    gp.set_latitude(37.7749).set_longitude(-122.4194).set_elevation(16.0f);

    integration::AddressArrowRow addr;
    addr.set_street("1 Market St").set_city("San Francisco").set_country("US");

    integration::LocationArrowRow loc;
    loc.set_point(gp).set_address(addr).set_name("HQ");

    auto encoded_direct = loc.Encode();
    fletcher::RowCodec codec(integration::LocationArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(loc.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}

TEST_CASE("EncodeTo: repeated scalars and structs — byte identical") {
    integration::PlayerArrowRow p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    integration::TeamArrowRow team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob", "Carol"})
        .set_scores({95.0, 87.5, 92.0})
        .set_roster({p1, p2});

    auto encoded_direct = team.Encode();
    fletcher::RowCodec codec(integration::TeamArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(team.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}

TEST_CASE("EncodeTo: map fields — byte identical") {
    integration::MetricsArrowRow m;
    m.set_resource_id("srv-1")
     .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
     .set_counters({{"requests", INT64_C(10000)}, {"errors", INT64_C(3)}});

    auto encoded_direct = m.Encode();
    fletcher::RowCodec codec(integration::MetricsArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(m.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}

TEST_CASE("EncodeTo: WKT + list<struct> + map + optional — byte identical") {
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

    auto encoded_direct = order.Encode();
    fletcher::RowCodec codec(integration::OrderArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(order.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}

TEST_CASE("EncodeTo: temporal WKT — byte identical") {
    integration::TimedEventArrowRow ev;
    ev.set_event_id("evt-001")
      .set_occurred_at(1'700'000'000'000'000'000LL)
      .set_elapsed(5'000'000'000LL)
      .set_score(9.5);

    auto encoded_direct = ev.Encode();
    fletcher::RowCodec codec(integration::TimedEventArrowRowSchema());
    auto encoded_codec = codec.EncodeRow(ev.ToScalars());

    REQUIRE(encoded_direct.size() == encoded_codec.size());
    CHECK(encoded_direct == encoded_codec);
}
