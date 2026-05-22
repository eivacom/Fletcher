// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// SensorReading — flat message covering all supported scalar proto types,
// non-optional and optional. Verifies:
//
// - Schema structure matches expectations (types, nullability)
// - Encode produces non-empty bytes
// - Round-trip through Codec preserves scalar values
// - Optional fields are null when not set, valid + correct when set

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <memory>

#include "simple.fletcher.pb.h"

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

// Decode an encoded row using the given schema via Codec. The bytes are
// kept alive in a file-local static because DecodeScalarFromReader hands
// out non-owning arrow::Buffer views into the input data.
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
    r.set_sensor_id(1)
        .set_temperature(0.0)
        .set_pressure(0.0f)
        .set_active(false)
        .set_location("")
        .set_payload("")
        .set_sequence(0u)
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
    ASSERT_EQ(scalars.size(), 10u);

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
    r.set_sensor_id(1)
        .set_temperature(0.0)
        .set_pressure(0.0f)
        .set_active(false)
        .set_location("")
        .set_payload("")
        .set_sequence(0u)
        .set_timestamp_ns(0LL);

    auto scalars = RoundTrip(r.Encode(), fletcher_gen::integration::SensorReadingSchema());
    EXPECT_FALSE(scalars[8]->is_valid);  // humidity
    EXPECT_FALSE(scalars[9]->is_valid);  // label
}

TEST(SensorReadingTest, OptionalFieldsValidWhenSet) {
    fletcher_gen::integration::SensorReading r;
    r.set_sensor_id(1)
        .set_temperature(0.0)
        .set_pressure(0.0f)
        .set_active(false)
        .set_location("")
        .set_payload("")
        .set_sequence(0u)
        .set_timestamp_ns(0LL)
        .set_humidity(55.3)
        .set_label("humid");

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
