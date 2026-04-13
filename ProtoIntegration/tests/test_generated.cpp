#include <catch2/catch_test_macros.hpp>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <pubsub/pubsub_provider.hpp>
#include <pubsub/owned_schema.hpp>
#include <pubsub/write_buffer.hpp>
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

// View headers (Arrow C++ dependent, server-side)
#include "simple.fletcher.view.pb.h"
#include "nested.fletcher.view.pb.h"
#include "collections.fletcher.view.pb.h"
#include "maps.fletcher.view.pb.h"
#include "complex.fletcher.view.pb.h"

// Helper: import an OwnedSchema (nanoarrow) to shared_ptr<arrow::Schema>.
static std::shared_ptr<arrow::Schema> ImportNano(fletcher::OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    REQUIRE(result.ok());
    return *result;
}

// Helper: decode an encoded row using the given schema via PositionalCodec.
// The encoded data is kept alive in a static to prevent dangling Buffer
// pointers — DecodeScalarFromReader creates non-owning Buffers into the
// input data.
fletcher::ArrowRow RoundTrip(
    fletcher::EncodedRow encoded,
    fletcher::OwnedSchema nano_schema) {
    static fletcher::EncodedRow kept_alive;
    kept_alive = std::move(encoded);
    auto schema = ImportNano(std::move(nano_schema));
    fletcher::PositionalCodec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

// =============================================================================
// simple.proto — SensorReadingArrowRow
// =============================================================================

TEST_CASE("SensorReading: schema structure") {
    auto schema = ImportNano(integration::SensorReadingArrowRowSchema());
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

TEST_CASE("SensorReading: roundtrip -scalar values") {
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
    auto schema = ImportNano(integration::TimedEventArrowRowSchema());
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

TEST_CASE("TimedEvent: roundtrip -WKT values") {
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
    auto schema = ImportNano(integration::GeoPointArrowRowSchema());
    REQUIRE(schema->num_fields() == 3);
    CHECK(schema->field(0)->name() == "latitude");
    CHECK(schema->field(0)->type()->id() == arrow::Type::DOUBLE);
    CHECK(schema->field(1)->name() == "longitude");
    CHECK(schema->field(2)->name() == "elevation");
    CHECK(schema->field(2)->type()->id() == arrow::Type::FLOAT);
}

TEST_CASE("Location: schema embeds struct fields") {
    auto schema = ImportNano(integration::LocationArrowRowSchema());
    REQUIRE(schema->num_fields() == 3);

    CHECK(schema->field(0)->name() == "point");
    CHECK(schema->field(0)->type()->id() == arrow::Type::STRUCT);
    CHECK_FALSE(schema->field(0)->nullable());

    CHECK(schema->field(1)->name() == "address");
    CHECK(schema->field(1)->type()->id() == arrow::Type::STRUCT);

    CHECK(schema->field(2)->name() == "name");
    CHECK(schema->field(2)->type()->id() == arrow::Type::STRING);
}

TEST_CASE("Location: roundtrip -nested structs") {
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
    auto schema = ImportNano(integration::TeamArrowRowSchema());
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

TEST_CASE("Team: roundtrip -repeated scalars and structs") {
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
    auto schema = ImportNano(integration::MetricsArrowRowSchema());
    REQUIRE(schema->num_fields() == 3);

    CHECK(schema->field(0)->name() == "resource_id");

    CHECK(schema->field(1)->name() == "gauges");
    CHECK(schema->field(1)->type()->id() == arrow::Type::MAP);
    CHECK_FALSE(schema->field(1)->nullable());

    CHECK(schema->field(2)->name() == "counters");
    CHECK(schema->field(2)->type()->id() == arrow::Type::MAP);
}

TEST_CASE("Metrics: roundtrip -map fields") {
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
    auto schema = ImportNano(integration::OrderArrowRowSchema());
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

TEST_CASE("Order: roundtrip -full complex row") {
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
        fletcher::OwnedSchema schema;
    };

    struct PublishedMsg {
        std::vector<std::string> segments;
        std::vector<uint8_t> encoded;
        fletcher::Attachments attachments;
    };

    std::vector<CreatedTopic> created_topics;
    std::vector<PublishedMsg> published;
    std::map<std::vector<std::string>, SubscribeCallback> subscribers;

    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema schema) override {
        created_topics.push_back({segments, std::move(schema)});
    }

    void Publish(const std::vector<std::string>& segments,
                 RowEncoder encoder,
                 const fletcher::Attachments& attachments) override {
        std::vector<uint8_t> buf;
        fletcher::VectorWriteBuffer wb(buf);
        encoder(wb);
        auto it = subscribers.find(segments);
        if (it != subscribers.end())
            it->second(buf.data(), buf.size(), attachments);
        published.push_back({segments, std::move(buf), attachments});
    }

    fletcher::SubscriptionResult Subscribe(
        const std::vector<std::string>& segments,
        SubscribeCallback callback) override {
        subscribers[segments] = std::move(callback);
        // Return a schema if we have one for this topic.
        for (const auto& ct : created_topics) {
            if (ct.segments == segments)
                return {fletcher::OwnedSchema::DeepCopy(ct.schema.get())};
        }
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
    auto schema = ImportNano(
        fletcher::OwnedSchema::DeepCopy(mock->created_topics[0].schema.get()));
    CHECK(schema->num_fields() == 4);
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
    CHECK_FALSE(mock->published[0].encoded.empty());
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

TEST_CASE("Subscriber: receives typed message from published rows") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    integration::TelemetryArrowRow received;
    sub.Subscribe([&](integration::TelemetryArrowRow msg, fletcher::Attachments) {
        received = std::move(msg);
    });

    integration::TelemetryArrowRow row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    CHECK(received.device_id() == 42);
    CHECK(received.value() == 3.14);
    CHECK(received.timestamp() == 1000LL);
    CHECK(received.metric_name() == "cpu");
}

TEST_CASE("Subscriber: unsubscribe stops delivery") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    int count = 0;
    sub.Subscribe([&](integration::TelemetryArrowRow, fletcher::Attachments) { ++count; });

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

    integration::TelemetryArrowRow received;
    fletcher::Attachments received_att;
    sub.Subscribe([&](integration::TelemetryArrowRow msg, fletcher::Attachments att) {
        received = std::move(msg);
        received_att = std::move(att);
    });

    integration::TelemetryArrowRow row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    pub.Publish(row, {{"image", blob}});

    CHECK(received.device_id() == 42);
    REQUIRE(received_att.size() == 1);
    REQUIRE(received_att.count("image") == 1);
    CHECK(*received_att.at("image") == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("Publisher: publish without attachments has empty attachments") {
    auto mock = std::make_shared<MockPubSubProvider>();
    integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    integration::TelemetryArrowRow row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");
    pub.Publish(row);

    REQUIRE(mock->published.size() == 1);
    CHECK(mock->published[0].attachments.empty());
    CHECK_FALSE(mock->published[0].encoded.empty());
}

// =============================================================================
// Native encode/decode roundtrip — verify the generated decode constructor
// produces the same values that were encoded.
// =============================================================================

TEST_CASE("Native roundtrip: simple scalars") {
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

    integration::SensorReadingArrowRow decoded(r.Encode());

    CHECK(decoded.sensor_id() == 42);
    CHECK(decoded.temperature() == 23.5);
    CHECK(decoded.pressure() == 1013.25f);
    CHECK(decoded.active() == true);
    CHECK(decoded.location() == "Room 101");
    CHECK(decoded.payload() == "\xDE\xAD\xBE\xEF");
    CHECK(decoded.sequence() == 7u);
    CHECK(decoded.timestamp_ns() == 1'000'000'000LL);
    REQUIRE(decoded.humidity().has_value());
    CHECK(*decoded.humidity() == 55.3);
    REQUIRE(decoded.label().has_value());
    CHECK(*decoded.label() == "humid");
}

TEST_CASE("Native roundtrip: optional nulls") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL);
    // humidity and label intentionally left null

    integration::SensorReadingArrowRow decoded(r.Encode());
    CHECK_FALSE(decoded.humidity().has_value());
    CHECK_FALSE(decoded.label().has_value());
}

TEST_CASE("Native roundtrip: nested structs") {
    integration::GeoPointArrowRow gp;
    gp.set_latitude(37.7749).set_longitude(-122.4194).set_elevation(16.0f);

    integration::AddressArrowRow addr;
    addr.set_street("1 Market St").set_city("San Francisco").set_country("US");

    integration::LocationArrowRow loc;
    loc.set_point(gp).set_address(addr).set_name("HQ");

    integration::LocationArrowRow decoded(loc.Encode());
    CHECK(decoded.point().latitude() == 37.7749);
    CHECK(decoded.point().longitude() == -122.4194);
    CHECK(decoded.point().elevation() == 16.0f);
    CHECK(decoded.address().street() == "1 Market St");
    CHECK(decoded.address().city() == "San Francisco");
    CHECK(decoded.name() == "HQ");
}

TEST_CASE("Native roundtrip: repeated scalars and structs") {
    integration::PlayerArrowRow p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    integration::TeamArrowRow team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob", "Carol"})
        .set_scores({95.0, 87.5, 92.0})
        .set_roster({p1, p2});

    integration::TeamArrowRow decoded(team.Encode());
    CHECK(decoded.name() == "Alpha");
    REQUIRE(decoded.members().size() == 3);
    CHECK(decoded.members()[0] == "Alice");
    CHECK(decoded.members()[1] == "Bob");
    CHECK(decoded.members()[2] == "Carol");
    REQUIRE(decoded.scores().size() == 3);
    CHECK(decoded.scores()[0] == 95.0);
    REQUIRE(decoded.roster().size() == 2);
    CHECK(decoded.roster()[0].name() == "Alice");
    CHECK(decoded.roster()[0].level() == 5);
    CHECK(decoded.roster()[1].name() == "Bob");
}

TEST_CASE("Native roundtrip: map fields") {
    integration::MetricsArrowRow m;
    m.set_resource_id("srv-1")
     .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
     .set_counters({{"requests", INT64_C(10000)}, {"errors", INT64_C(3)}});

    integration::MetricsArrowRow decoded(m.Encode());
    CHECK(decoded.resource_id() == "srv-1");
    REQUIRE(decoded.gauges().size() == 2);
    REQUIRE(decoded.counters().size() == 2);
}

TEST_CASE("Native roundtrip: complex - WKT + list<struct> + map + optional") {
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

    integration::OrderArrowRow decoded(order.Encode());
    CHECK(decoded.order_id() == "ORD-12345");
    CHECK(decoded.created_at() == 1'700'000'000'000'000'000LL);
    REQUIRE(decoded.items().size() == 2);
    CHECK(decoded.items()[0].product_id() == "SKU-001");
    CHECK(decoded.items()[0].quantity() == 2);
    CHECK(decoded.items()[1].note().has_value());
    CHECK(*decoded.items()[1].note() == "gift wrap");
    REQUIRE(decoded.tags().size() == 2);
    REQUIRE(decoded.customer_note().has_value());
    CHECK(*decoded.customer_note() == "Leave at door");
}

TEST_CASE("Native roundtrip: temporal WKT") {
    integration::TimedEventArrowRow ev;
    ev.set_event_id("evt-001")
      .set_occurred_at(1'700'000'000'000'000'000LL)
      .set_elapsed(5'000'000'000LL)
      .set_score(9.5);

    integration::TimedEventArrowRow decoded(ev.Encode());
    CHECK(decoded.event_id() == "evt-001");
    CHECK(decoded.occurred_at() == 1'700'000'000'000'000'000LL);
    CHECK(decoded.elapsed() == 5'000'000'000LL);
    REQUIRE(decoded.score().has_value());
    CHECK(*decoded.score() == 9.5);
    CHECK_FALSE(decoded.label().has_value());
}

// =============================================================================
// View class tests — encode → decode to ArrowRow → construct view → verify
// =============================================================================

TEST_CASE("View: SensorReading from ArrowRow") {
    integration::SensorReadingArrowRow r;
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

    auto row = RoundTrip(r.Encode(), integration::SensorReadingArrowRowSchema());
    integration::SensorReadingArrowRowView view(std::move(row));

    CHECK(view.sensor_id() == 42);
    CHECK(view.temperature() == 23.5);
    CHECK(view.pressure() == 1013.25f);
    CHECK(view.active() == true);
    CHECK(view.location() == "roof");
    CHECK(view.payload() == std::string_view("\x01\x02\x03", 3));
    CHECK(view.sequence() == 100);
    CHECK(view.timestamp_ns() == 1'700'000'000'000'000'000LL);
    REQUIRE(view.humidity().has_value());
    CHECK(*view.humidity() == 65.0);
    REQUIRE(view.label().has_value());
    CHECK(*view.label() == "test-label");
}

TEST_CASE("View: SensorReading with nulls") {
    integration::SensorReadingArrowRow r;
    r.set_sensor_id(1)
     .set_temperature(0.0)
     .set_pressure(0.0f)
     .set_active(false)
     .set_location("")
     .set_payload("")
     .set_sequence(0)
     .set_timestamp_ns(0);
    // humidity and label left as nullopt

    auto row = RoundTrip(r.Encode(), integration::SensorReadingArrowRowSchema());
    integration::SensorReadingArrowRowView view(std::move(row));

    CHECK(view.sensor_id() == 1);
    CHECK_FALSE(view.humidity().has_value());
    CHECK_FALSE(view.label().has_value());
}

TEST_CASE("View: nested Location from ArrowRow") {
    integration::GeoPointArrowRow pt;
    pt.set_latitude(55.6761).set_longitude(12.5683).set_elevation(10.0f);

    integration::AddressArrowRow addr;
    addr.set_street("Nyhavn 1").set_city("Copenhagen").set_country("Denmark");

    integration::LocationArrowRow loc;
    loc.set_name("Office").set_point(pt).set_address(addr);

    auto row = RoundTrip(loc.Encode(), integration::LocationArrowRowSchema());
    integration::LocationArrowRowView view(std::move(row));

    CHECK(view.name() == "Office");
    auto pv = view.point();
    CHECK(pv.latitude() == 55.6761);
    CHECK(pv.longitude() == 12.5683);
    CHECK(pv.elevation() == 10.0f);
    auto av = view.address();
    CHECK(av.street() == "Nyhavn 1");
    CHECK(av.city() == "Copenhagen");
    CHECK(av.country() == "Denmark");
}
