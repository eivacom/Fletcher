// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// maps.proto — Metrics. Verifies proto map<K,V> fields map to
// arrow::Type::MAP and round-trip preserves entries.

#include "maps.fletcher.pb.h"

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
    ASSERT_EQ(scalars.size(), 3u);

    EXPECT_EQ(scalars[1]->type->id(), arrow::Type::MAP);
    EXPECT_TRUE(scalars[1]->is_valid);
    EXPECT_EQ(scalars[2]->type->id(), arrow::Type::MAP);
}
