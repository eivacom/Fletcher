#include <gtest/gtest.h>
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <pubsub/pubsub.hpp>
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
    if (!result.ok()) { ADD_FAILURE() << "ImportSchema failed"; return nullptr; }
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
    if (!schema) { ADD_FAILURE() << "RoundTrip: ImportNano failed"; return {}; }
    fletcher::PositionalCodec codec(std::move(schema));
    return codec.DecodeRow(kept_alive);
}

// =============================================================================
// simple.proto — SensorReadingArrowRow
// =============================================================================

TEST(SensorReadingTest, SchemaStructure) {
    auto schema = ImportNano(fletcher_gen::integration::SensorReadingSchema());
    ASSERT_EQ(schema->num_fields(), 10);

    // Non-optional scalars are not nullable.
    EXPECT_EQ(schema->field(0)->name(), "sensor_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::INT32);
    EXPECT_FALSE(schema->field(0)->nullable());

    EXPECT_EQ(schema->field(1)->name(), "temperature");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_FALSE(schema->field(1)->nullable());

    EXPECT_EQ(schema->field(3)->name(), "active");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::BOOL);

    EXPECT_EQ(schema->field(4)->name(), "location");
    EXPECT_EQ(schema->field(4)->type()->id(), arrow::Type::STRING);

    EXPECT_EQ(schema->field(5)->name(), "payload");
    EXPECT_EQ(schema->field(5)->type()->id(), arrow::Type::BINARY);

    // optional fields are nullable.
    EXPECT_EQ(schema->field(8)->name(), "humidity");
    EXPECT_EQ(schema->field(8)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_TRUE(schema->field(8)->nullable());

    EXPECT_EQ(schema->field(9)->name(), "label");
    EXPECT_EQ(schema->field(9)->type()->id(), arrow::Type::STRING);
    EXPECT_TRUE(schema->field(9)->nullable());
}

TEST(SensorReadingTest, EncodeIsNonEmpty) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL);
    EXPECT_FALSE(r.Encode().empty());
}

TEST(SensorReadingTest, RoundtripScalarValues) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(42)
     .set_temperature(23.5)
     .set_pressure(1013.25f)
     .set_active(true)
     .set_location("Room 101")
     .set_payload("\xDE\xAD\xBE\xEF")
     .set_sequence(7u)
     .set_timestamp_ns(1'000'000'000LL);

    auto scalars = RoundTrip(r.Encode(), fletcher_gen::integration::SensorReadingSchema());
    ASSERT_EQ(scalars.size(), 10);

    auto* id = dynamic_cast<arrow::Int32Scalar*>(scalars[0].get());
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->value, 42);

    auto* temp = dynamic_cast<arrow::DoubleScalar*>(scalars[1].get());
    ASSERT_NE(temp, nullptr);
    EXPECT_DOUBLE_EQ(temp->value, 23.5);

    auto* act = dynamic_cast<arrow::BooleanScalar*>(scalars[3].get());
    ASSERT_NE(act, nullptr);
    EXPECT_EQ(act->value, true);

    auto* loc = dynamic_cast<arrow::StringScalar*>(scalars[4].get());
    ASSERT_NE(loc, nullptr);
    EXPECT_EQ(loc->value->ToString(), "Room 101");

    auto* ts = dynamic_cast<arrow::Int64Scalar*>(scalars[7].get());
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->value, 1'000'000'000LL);
}

TEST(SensorReadingTest, OptionalFieldsNullWhenNotSet) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL);

    auto scalars = RoundTrip(r.Encode(), fletcher_gen::integration::SensorReadingSchema());
    EXPECT_FALSE(scalars[8]->is_valid);  // humidity
    EXPECT_FALSE(scalars[9]->is_valid);  // label
}

TEST(SensorReadingTest, OptionalFieldsValidWhenSet) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
     .set_timestamp_ns(0LL).set_humidity(55.3).set_label("humid");

    auto scalars = RoundTrip(r.Encode(), fletcher_gen::integration::SensorReadingSchema());
    ASSERT_TRUE(scalars[8]->is_valid);
    ASSERT_TRUE(scalars[9]->is_valid);

    auto* hum = dynamic_cast<arrow::DoubleScalar*>(scalars[8].get());
    ASSERT_NE(hum, nullptr);
    EXPECT_DOUBLE_EQ(hum->value, 55.3);

    auto* lbl = dynamic_cast<arrow::StringScalar*>(scalars[9].get());
    ASSERT_NE(lbl, nullptr);
    EXPECT_EQ(lbl->value->ToString(), "humid");
}

// =============================================================================
// temporal.proto — TimedEventArrowRow
// =============================================================================

TEST(TimedEventTest, SchemaStructure) {
    auto schema = ImportNano(fletcher_gen::integration::TimedEventSchema());
    ASSERT_EQ(schema->num_fields(), 5);

    EXPECT_EQ(schema->field(0)->name(), "event_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);
    EXPECT_FALSE(schema->field(0)->nullable());

    // google.protobuf.Timestamp → timestamp(NANO)
    EXPECT_EQ(schema->field(1)->name(), "occurred_at");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::TIMESTAMP);
    EXPECT_FALSE(schema->field(1)->nullable());

    // google.protobuf.Duration → duration(NANO)
    EXPECT_EQ(schema->field(2)->name(), "elapsed");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::DURATION);
    EXPECT_FALSE(schema->field(2)->nullable());

    // DoubleValue wrapper → nullable double
    EXPECT_EQ(schema->field(3)->name(), "score");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_TRUE(schema->field(3)->nullable());

    // StringValue wrapper → nullable string
    EXPECT_EQ(schema->field(4)->name(), "label");
    EXPECT_EQ(schema->field(4)->type()->id(), arrow::Type::STRING);
    EXPECT_TRUE(schema->field(4)->nullable());
}

TEST(TimedEventTest, RoundtripWktValues) {
    fletcher_gen::integration::TimedEvent ev;
    ev.set_event_id("evt-001")
      .set_occurred_at(1'700'000'000'000'000'000LL)
      .set_elapsed(5'000'000'000LL)
      .set_score(9.5);
    // label intentionally left unset → null

    auto scalars = RoundTrip(ev.Encode(), fletcher_gen::integration::TimedEventSchema());
    ASSERT_EQ(scalars.size(), 5);

    auto* ts = dynamic_cast<arrow::TimestampScalar*>(scalars[1].get());
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->value, 1'700'000'000'000'000'000LL);

    auto* dur = dynamic_cast<arrow::DurationScalar*>(scalars[2].get());
    ASSERT_NE(dur, nullptr);
    EXPECT_EQ(dur->value, 5'000'000'000LL);

    auto* sc = dynamic_cast<arrow::DoubleScalar*>(scalars[3].get());
    ASSERT_NE(sc, nullptr);
    EXPECT_TRUE(sc->is_valid);
    EXPECT_DOUBLE_EQ(sc->value, 9.5);

    // label not set → null scalar
    EXPECT_FALSE(scalars[4]->is_valid);
}

// =============================================================================
// nested.proto — GeoPointArrowRow, AddressArrowRow, LocationArrowRow
// =============================================================================

TEST(NestedProtoTest, GeoPointSchemaIsFlatScalars) {
    auto schema = ImportNano(fletcher_gen::integration::GeoPointSchema());
    ASSERT_EQ(schema->num_fields(), 3);
    EXPECT_EQ(schema->field(0)->name(), "latitude");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_EQ(schema->field(1)->name(), "longitude");
    EXPECT_EQ(schema->field(2)->name(), "elevation");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::FLOAT);
}

TEST(NestedProtoTest, LocationSchemaEmbedsStructFields) {
    auto schema = ImportNano(fletcher_gen::integration::LocationSchema());
    ASSERT_EQ(schema->num_fields(), 3);

    EXPECT_EQ(schema->field(0)->name(), "point");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRUCT);
    EXPECT_FALSE(schema->field(0)->nullable());

    EXPECT_EQ(schema->field(1)->name(), "address");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::STRUCT);

    EXPECT_EQ(schema->field(2)->name(), "name");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::STRING);
}

TEST(NestedProtoTest, LocationRoundtripNestedStructs) {
    fletcher_gen::integration::GeoPoint gp;
    gp.set_latitude(37.7749).set_longitude(-122.4194).set_elevation(16.0f);

    fletcher_gen::integration::Address addr;
    addr.set_street("1 Market St").set_city("San Francisco").set_country("US");

    fletcher_gen::integration::Location loc;
    loc.set_point(gp).set_address(addr).set_name("HQ");

    auto scalars = RoundTrip(loc.Encode(), fletcher_gen::integration::LocationSchema());
    ASSERT_EQ(scalars.size(), 3);

    EXPECT_EQ(scalars[0]->type->id(), arrow::Type::STRUCT);
    EXPECT_TRUE(scalars[0]->is_valid);

    EXPECT_EQ(scalars[1]->type->id(), arrow::Type::STRUCT);
    EXPECT_TRUE(scalars[1]->is_valid);

    auto* name = dynamic_cast<arrow::StringScalar*>(scalars[2].get());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->value->ToString(), "HQ");
}

// =============================================================================
// collections.proto — PlayerArrowRow, TeamArrowRow
// =============================================================================

TEST(CollectionProtoTest, TeamSchemaHasListFields) {
    auto schema = ImportNano(fletcher_gen::integration::TeamSchema());
    ASSERT_EQ(schema->num_fields(), 4);

    EXPECT_EQ(schema->field(0)->name(), "name");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);

    EXPECT_EQ(schema->field(1)->name(), "members");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::LIST);
    EXPECT_FALSE(schema->field(1)->nullable());

    EXPECT_EQ(schema->field(2)->name(), "scores");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::LIST);

    EXPECT_EQ(schema->field(3)->name(), "roster");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::LIST);
}

TEST(CollectionProtoTest, TeamRoundtripRepeatedScalarsAndStructs) {
    fletcher_gen::integration::Player p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    fletcher_gen::integration::Team team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob", "Carol"})
        .set_scores({95.0, 87.5, 92.0})
        .set_roster({p1, p2});

    auto scalars = RoundTrip(team.Encode(), fletcher_gen::integration::TeamSchema());
    ASSERT_EQ(scalars.size(), 4);

    auto* name = dynamic_cast<arrow::StringScalar*>(scalars[0].get());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->value->ToString(), "Alpha");

    auto* members = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    ASSERT_NE(members, nullptr);
    EXPECT_EQ(members->value->length(), 3);

    auto* scores = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(scores, nullptr);
    EXPECT_EQ(scores->value->length(), 3);

    auto* roster = dynamic_cast<arrow::ListScalar*>(scalars[3].get());
    ASSERT_NE(roster, nullptr);
    EXPECT_EQ(roster->value->length(), 2);
}

TEST(CollectionProtoTest, TeamEmptyRepeatedFieldsProduceEmptyLists) {
    fletcher_gen::integration::Team team;
    team.set_name("Empty");

    auto scalars = RoundTrip(team.Encode(), fletcher_gen::integration::TeamSchema());
    ASSERT_EQ(scalars.size(), 4);

    auto* members = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    ASSERT_NE(members, nullptr);
    EXPECT_EQ(members->value->length(), 0);
}

// =============================================================================
// maps.proto — MetricsArrowRow
// =============================================================================

TEST(MapProtoTest, MetricsSchemaHasMapFields) {
    auto schema = ImportNano(fletcher_gen::integration::MetricsSchema());
    ASSERT_EQ(schema->num_fields(), 3);

    EXPECT_EQ(schema->field(0)->name(), "resource_id");

    EXPECT_EQ(schema->field(1)->name(), "gauges");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::MAP);
    EXPECT_FALSE(schema->field(1)->nullable());

    EXPECT_EQ(schema->field(2)->name(), "counters");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::MAP);
}

TEST(MapProtoTest, MetricsRoundtripMapFields) {
    fletcher_gen::integration::Metrics m;
    m.set_resource_id("srv-1")
     .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
     .set_counters({{"requests", INT64_C(10000)}, {"errors", INT64_C(3)}});

    auto scalars = RoundTrip(m.Encode(), fletcher_gen::integration::MetricsSchema());
    ASSERT_EQ(scalars.size(), 3);

    EXPECT_EQ(scalars[1]->type->id(), arrow::Type::MAP);
    EXPECT_TRUE(scalars[1]->is_valid);
    EXPECT_EQ(scalars[2]->type->id(), arrow::Type::MAP);
}

// =============================================================================
// complex.proto — OrderItemArrowRow, OrderArrowRow
// =============================================================================

TEST(OrderProtoTest, OrderSchemaCombinesWktListStructMapAndOptional) {
    auto schema = ImportNano(fletcher_gen::integration::OrderSchema());
    ASSERT_EQ(schema->num_fields(), 5);

    EXPECT_EQ(schema->field(0)->name(), "order_id");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::STRING);
    EXPECT_FALSE(schema->field(0)->nullable());

    EXPECT_EQ(schema->field(1)->name(), "created_at");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::TIMESTAMP);
    EXPECT_FALSE(schema->field(1)->nullable());

    EXPECT_EQ(schema->field(2)->name(), "items");
    EXPECT_EQ(schema->field(2)->type()->id(), arrow::Type::LIST);
    EXPECT_FALSE(schema->field(2)->nullable());

    EXPECT_EQ(schema->field(3)->name(), "tags");
    EXPECT_EQ(schema->field(3)->type()->id(), arrow::Type::MAP);

    EXPECT_EQ(schema->field(4)->name(), "customer_note");
    EXPECT_EQ(schema->field(4)->type()->id(), arrow::Type::STRING);
    EXPECT_TRUE(schema->field(4)->nullable());
}

TEST(OrderProtoTest, OrderRoundtripFullComplexRow) {
    fletcher_gen::integration::OrderItem item1, item2;
    item1.set_product_id("SKU-001").set_quantity(2).set_unit_price(9.99);
    item2.set_product_id("SKU-002").set_quantity(1).set_unit_price(24.99)
         .set_note("gift wrap");

    fletcher_gen::integration::Order order;
    order.set_order_id("ORD-12345")
         .set_created_at(1'700'000'000'000'000'000LL)
         .set_items({item1, item2})
         .set_tags({{"priority", 1}, {"region", 3}})
         .set_customer_note("Leave at door");

    auto scalars = RoundTrip(order.Encode(), fletcher_gen::integration::OrderSchema());
    ASSERT_EQ(scalars.size(), 5);

    auto* oid = dynamic_cast<arrow::StringScalar*>(scalars[0].get());
    ASSERT_NE(oid, nullptr);
    EXPECT_EQ(oid->value->ToString(), "ORD-12345");

    auto* ts = dynamic_cast<arrow::TimestampScalar*>(scalars[1].get());
    ASSERT_NE(ts, nullptr);
    EXPECT_EQ(ts->value, 1'700'000'000'000'000'000LL);

    auto* items = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->value->length(), 2);

    EXPECT_EQ(scalars[3]->type->id(), arrow::Type::MAP);

    // customer_note was set → valid
    ASSERT_TRUE(scalars[4]->is_valid);
    auto* note = dynamic_cast<arrow::StringScalar*>(scalars[4].get());
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->value->ToString(), "Leave at door");
}

TEST(OrderProtoTest, CustomerNoteNullWhenNotSet) {
    fletcher_gen::integration::Order order;
    order.set_order_id("ORD-0").set_created_at(0LL);

    auto scalars = RoundTrip(order.Encode(), fletcher_gen::integration::OrderSchema());
    ASSERT_EQ(scalars.size(), 5);
    EXPECT_FALSE(scalars[4]->is_valid);
}

TEST(OrderProtoTest, OrderItemOptionalNoteNullThenValid) {
    fletcher_gen::integration::OrderItem item;
    item.set_product_id("SKU-X").set_quantity(1).set_unit_price(5.0);

    {
        auto scalars = RoundTrip(item.Encode(), fletcher_gen::integration::OrderItemSchema());
        EXPECT_FALSE(scalars[3]->is_valid);  // note not set
    }

    item.set_note("fragile");
    {
        auto scalars = RoundTrip(item.Encode(), fletcher_gen::integration::OrderItemSchema());
        EXPECT_TRUE(scalars[3]->is_valid);
        auto* n = dynamic_cast<arrow::StringScalar*>(scalars[3].get());
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->value->ToString(), "fragile");
    }
}

// =============================================================================
// pubsub.proto — Publisher and Subscriber classes
// =============================================================================

namespace {

// Minimal mock that records calls and delivers published rows to subscribers.
class MockPubSubProvider : public fletcher::PubSub {
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
                     fletcher::OwnedSchema schema,
                     std::any /*config*/) override {
        created_topics.push_back({segments, std::move(schema)});
    }

    void Publish(const std::vector<std::string>& segments,
                 RowEncoder encoder,
                 const fletcher::Attachments& attachments) override {
        std::vector<uint8_t> buf;
        fletcher::VectorWriteBuffer wb(buf);
        encoder(wb);
        fletcher::SharedSchema sp;
        for (const auto& ct : created_topics) {
            if (ct.segments == segments) {
                sp = fletcher::MakeSharedSchema(fletcher::OwnedSchema::DeepCopy(ct.schema.get()));
                break;
            }
        }
        auto it = subscribers.find(segments);
        if (it != subscribers.end())
            it->second(buf.data(), buf.size(), sp, attachments);
        published.push_back({segments, std::move(buf), attachments});
    }

    fletcher::SubscriptionResult Subscribe(
        const std::vector<std::string>& segments,
        SubscribeCallback callback,
        std::any /*config*/) override {
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

TEST(PubSubProtoTest, PublisherConstructionCreatesTopicWithCorrectSchema) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    ASSERT_EQ(mock->created_topics.size(), 1);
    EXPECT_EQ(mock->created_topics[0].segments,
          (std::vector<std::string>{"integration", "TelemetryFeed", "TelemetryStream"}));
    auto schema = ImportNano(
        fletcher::OwnedSchema::DeepCopy(mock->created_topics[0].schema.get()));
    EXPECT_EQ(schema->num_fields(), 4);
}

TEST(PubSubProtoTest, PublishEncodesAndDeliversToProvider) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    fletcher_gen::integration::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    ASSERT_EQ(mock->published.size(), 1);
    EXPECT_EQ(mock->published[0].segments,
          (std::vector<std::string>{"integration", "TelemetryFeed", "TelemetryStream"}));
    EXPECT_FALSE(mock->published[0].encoded.empty());
}

TEST(PubSubProtoTest, MultiplePublishesAccumulate) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    fletcher_gen::integration::Telemetry r1, r2, r3;
    r1.set_device_id(1).set_value(1.0).set_timestamp(100LL).set_metric_name("a");
    r2.set_device_id(2).set_value(2.0).set_timestamp(200LL).set_metric_name("b");
    r3.set_device_id(3).set_value(3.0).set_timestamp(300LL).set_metric_name("c");

    pub.Publish(r1);
    pub.Publish(r2);
    pub.Publish(r3);

    EXPECT_EQ(mock->published.size(), 3);
}

// ---- Subscriber tests ----------------------------------------------------

TEST(PubSubProtoTest, SubscriberConstructionDoesNotCreateTopic) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    // Subscriber-only processes no longer call CreateTopic; the schema
    // is discovered from the provider when Subscribe() is called.
    EXPECT_TRUE(mock->created_topics.empty());
}

TEST(PubSubProtoTest, SubscriberReceivesTypedMessageFromPublishedRows) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher_gen::integration::Telemetry received;
    sub.Subscribe([&](fletcher_gen::integration::Telemetry msg, fletcher::Attachments) {
        received = std::move(msg);
    });

    fletcher_gen::integration::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    EXPECT_EQ(received.device_id(), 42);
    EXPECT_DOUBLE_EQ(received.value(), 3.14);
    EXPECT_EQ(received.timestamp(), 1000LL);
    EXPECT_EQ(received.metric_name(), "cpu");
}

TEST(PubSubProtoTest, UnsubscribeStopsDelivery) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    int count = 0;
    sub.Subscribe([&](fletcher_gen::integration::Telemetry, fletcher::Attachments) { ++count; });

    fletcher_gen::integration::Telemetry row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");

    pub.Publish(row);
    EXPECT_EQ(count, 1);

    sub.Unsubscribe();
    pub.Publish(row);
    EXPECT_EQ(count, 1);  // no delivery after unsubscribe
}

TEST(PubSubProtoTest, PublishWithAttachmentsDeliversBlobToSubscriber) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher_gen::integration::Telemetry received;
    fletcher::Attachments received_att;
    sub.Subscribe([&](fletcher_gen::integration::Telemetry msg, fletcher::Attachments att) {
        received = std::move(msg);
        received_att = std::move(att);
    });

    fletcher_gen::integration::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    pub.Publish(row, {{"image", blob}});

    EXPECT_EQ(received.device_id(), 42);
    ASSERT_EQ(received_att.size(), 1);
    ASSERT_EQ(received_att.count("image"), 1);
    EXPECT_EQ(*received_att.at("image"), (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(PubSubProtoTest, PublishWithoutAttachmentsHasEmptyAttachments) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    fletcher_gen::integration::Telemetry row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");
    pub.Publish(row);

    ASSERT_EQ(mock->published.size(), 1);
    EXPECT_TRUE(mock->published[0].attachments.empty());
    EXPECT_FALSE(mock->published[0].encoded.empty());
}

// =============================================================================
// Native encode/decode roundtrip — verify the generated decode constructor
// produces the same values that were encoded.
// =============================================================================

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
    r.set_sensor_id(1).set_temperature(0.0).set_pressure(0.0f)
     .set_active(false).set_location("").set_payload("").set_sequence(0u)
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
    ASSERT_EQ(decoded.members().size(), 3);
    EXPECT_EQ(decoded.members()[0], "Alice");
    EXPECT_EQ(decoded.members()[1], "Bob");
    EXPECT_EQ(decoded.members()[2], "Carol");
    ASSERT_EQ(decoded.scores().size(), 3);
    EXPECT_DOUBLE_EQ(decoded.scores()[0], 95.0);
    ASSERT_EQ(decoded.roster().size(), 2);
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
    ASSERT_EQ(decoded.gauges().size(), 2);
    ASSERT_EQ(decoded.counters().size(), 2);
}

TEST(NativeRoundtripTest, ComplexWktListStructMapAndOptional) {
    fletcher_gen::integration::OrderItem item1, item2;
    item1.set_product_id("SKU-001").set_quantity(2).set_unit_price(9.99);
    item2.set_product_id("SKU-002").set_quantity(1).set_unit_price(24.99)
         .set_note("gift wrap");

    fletcher_gen::integration::Order order;
    order.set_order_id("ORD-12345")
         .set_created_at(1'700'000'000'000'000'000LL)
         .set_items({item1, item2})
         .set_tags({{"priority", 1}, {"region", 3}})
         .set_customer_note("Leave at door");

    fletcher_gen::integration::Order decoded(order.Encode());
    EXPECT_EQ(decoded.order_id(), "ORD-12345");
    EXPECT_EQ(decoded.created_at(), 1'700'000'000'000'000'000LL);
    ASSERT_EQ(decoded.items().size(), 2);
    EXPECT_EQ(decoded.items()[0].product_id(), "SKU-001");
    EXPECT_EQ(decoded.items()[0].quantity(), 2);
    ASSERT_TRUE(decoded.items()[1].note().has_value());
    EXPECT_EQ(*decoded.items()[1].note(), "gift wrap");
    ASSERT_EQ(decoded.tags().size(), 2);
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

// =============================================================================
// View class tests — encode → decode to ArrowRow → construct view → verify
// =============================================================================

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
