// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// nested.proto — GeoPoint, Address, Location. Verifies that nested
// proto messages map to arrow::Type::STRUCT and round-trip correctly
// through the recursive positional format (struct: NULL_BITFIELD then
// nested payloads).

#include "nested.fletcher.pb.h"

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
    ASSERT_EQ(scalars.size(), 3u);

    EXPECT_EQ(scalars[0]->type->id(), arrow::Type::STRUCT);
    EXPECT_TRUE(scalars[0]->is_valid);

    EXPECT_EQ(scalars[1]->type->id(), arrow::Type::STRUCT);
    EXPECT_TRUE(scalars[1]->is_valid);

    auto* name = dynamic_cast<arrow::StringScalar*>(scalars[2].get());
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(name->value->ToString(), "HQ");
}
