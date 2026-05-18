// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// collections.proto — Player, Team. Verifies repeated proto fields map
// to arrow::Type::LIST and round-trip with the right element counts,
// for repeated scalars, repeated structs, and empty repeated fields.

#include "collections.fletcher.pb.h"

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
    ASSERT_EQ(scalars.size(), 4u);

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
    ASSERT_EQ(scalars.size(), 4u);

    auto* members = dynamic_cast<arrow::ListScalar*>(scalars[1].get());
    ASSERT_NE(members, nullptr);
    EXPECT_EQ(members->value->length(), 0);
}
