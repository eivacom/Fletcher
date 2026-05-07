// complex.proto — Order, OrderItem. Combines well-known types
// (Timestamp), repeated structs (list of OrderItem), maps (tags),
// optional strings (customer_note, OrderItem.note). Verifies the
// full combination round-trips.

#include "complex.fletcher.pb.h"

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
    ASSERT_EQ(scalars.size(), 5u);

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

    ASSERT_TRUE(scalars[4]->is_valid);
    auto* note = dynamic_cast<arrow::StringScalar*>(scalars[4].get());
    ASSERT_NE(note, nullptr);
    EXPECT_EQ(note->value->ToString(), "Leave at door");
}

TEST(OrderProtoTest, CustomerNoteNullWhenNotSet) {
    fletcher_gen::integration::Order order;
    order.set_order_id("ORD-0").set_created_at(0LL);

    auto scalars = RoundTrip(order.Encode(), fletcher_gen::integration::OrderSchema());
    ASSERT_EQ(scalars.size(), 5u);
    EXPECT_FALSE(scalars[4]->is_valid);
}

TEST(OrderProtoTest, OrderItemOptionalNoteNullThenValid) {
    fletcher_gen::integration::OrderItem item;
    item.set_product_id("SKU-X").set_quantity(1).set_unit_price(5.0);

    {
        auto scalars = RoundTrip(item.Encode(), fletcher_gen::integration::OrderItemSchema());
        EXPECT_FALSE(scalars[3]->is_valid);
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
