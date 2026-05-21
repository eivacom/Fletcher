// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// TimedEvent — proto well-known types: google.protobuf.Timestamp,
// google.protobuf.Duration, plus the wrapper types DoubleValue and
// StringValue (which become nullable scalars). Verifies:
//
// - Schema maps Timestamp → arrow::Type::TIMESTAMP, Duration → DURATION
// - Wrapper types produce nullable Arrow scalars
// - Round-trip preserves nanoseconds-precision values

#include "temporal.fletcher.pb.h"

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/owned_schema.hpp>

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
    ASSERT_EQ(scalars.size(), 5u);

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

    EXPECT_FALSE(scalars[4]->is_valid);
}
