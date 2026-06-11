// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Regression test for the nested-nullability mismatch between the two emitters:
// the nanoarrow schema (<msg>Schema(), via GenerateSchemaFunction) and the
// ToArrowRow() free function (via GenerateToArrowRow). They must agree on the
// nullability of nested list "item" and map "key"/"value" child fields.
//
// Before the fix, the schema left list "item" / map "value" nullable (nanoarrow
// default) while ToArrowRow hard-built them non-nullable (field("item", t, false)).
// A schema-validating consumer comparing ToArrowRow(msg)[i]->type against
// Schema()->field(i)->type() with DataType::Equals then rejected every row with a
// nested list/map field. This test asserts the two are byte-for-byte equal, so it
// FAILS on the old generator and PASSES on the fixed one.
//
// Note: it deliberately exercises ToArrowRow() directly rather than a Codec
// round-trip — a decoded row is built from the schema and so always matches it,
// which would mask the bug.

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <memory>

#include "collections.fletcher.arrow.pb.h"
#include "collections.fletcher.pb.h"
#include "maps.fletcher.arrow.pb.h"
#include "maps.fletcher.pb.h"

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

// The core invariant: every scalar produced by ToArrowRow() carries exactly the
// type the schema declares for that field — including nested child nullability.
void ExpectRowTypesMatchSchema(const ArrowRow& row, const std::shared_ptr<arrow::Schema>& schema) {
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(static_cast<int>(row.size()), schema->num_fields());
    for (int i = 0; i < schema->num_fields(); ++i) {
        ASSERT_NE(row[static_cast<size_t>(i)], nullptr);
        ASSERT_NE(row[static_cast<size_t>(i)]->type, nullptr);
        EXPECT_TRUE(row[static_cast<size_t>(i)]->type->Equals(*schema->field(i)->type()))
            << "field " << i << " '" << schema->field(i)->name() << "': ToArrowRow type "
            << row[static_cast<size_t>(i)]->type->ToString() << " != schema type "
            << schema->field(i)->type()->ToString();
    }
}

}  // namespace

// Repeated scalar (list<utf8>, list<float64>) and repeated message (list<struct>).
TEST(NullabilityConsistencyTest, TeamToArrowRowTypesMatchSchema) {
    fletcher_gen::integration::Player p1, p2;
    p1.set_name("Alice").set_level(5);
    p2.set_name("Bob").set_level(3);

    fletcher_gen::integration::Team team;
    team.set_name("Alpha")
        .set_members({"Alice", "Bob"})
        .set_scores({95.0, 87.5})
        .set_roster({p1, p2});

    // ADL picks fletcher_gen::integration::ToArrowRow.
    ExpectRowTypesMatchSchema(ToArrowRow(team),
                              ImportNano(fletcher_gen::integration::TeamSchema()));
}

// map<string,double> and map<string,int64>.
TEST(NullabilityConsistencyTest, MetricsToArrowRowTypesMatchSchema) {
    fletcher_gen::integration::Metrics m;
    m.set_resource_id("srv-1")
        .set_gauges({{"cpu_pct", 45.2}, {"mem_pct", 72.1}})
        .set_counters({{"requests", INT64_C(10000)}});

    ExpectRowTypesMatchSchema(ToArrowRow(m),
                              ImportNano(fletcher_gen::integration::MetricsSchema()));
}

// Documents the chosen convention explicitly (Arrow-conventional):
// list "item" nullable; map "key" non-nullable; map "value" nullable.
TEST(NullabilityConsistencyTest, NestedChildNullabilityFollowsArrowConvention) {
    auto team = ImportNano(fletcher_gen::integration::TeamSchema());
    ASSERT_NE(team, nullptr);

    // members: list<utf8> — child 0 is the "item" field.
    const auto& members = team->field(1)->type();
    ASSERT_EQ(members->id(), arrow::Type::LIST);
    EXPECT_EQ(members->field(0)->name(), "item");
    EXPECT_TRUE(members->field(0)->nullable());

    // roster: list<struct> — item also nullable.
    const auto& roster = team->field(3)->type();
    ASSERT_EQ(roster->id(), arrow::Type::LIST);
    EXPECT_TRUE(roster->field(0)->nullable());

    auto metrics = ImportNano(fletcher_gen::integration::MetricsSchema());
    ASSERT_NE(metrics, nullptr);

    // gauges: map<utf8,float64> — entries struct holds key (non-null) and value (nullable).
    const auto& gauges = metrics->field(1)->type();
    ASSERT_EQ(gauges->id(), arrow::Type::MAP);
    const auto& entries = gauges->field(0)->type();  // "entries" struct
    ASSERT_EQ(entries->num_fields(), 2);
    EXPECT_EQ(entries->field(0)->name(), "key");
    EXPECT_FALSE(entries->field(0)->nullable());
    EXPECT_EQ(entries->field(1)->name(), "value");
    EXPECT_TRUE(entries->field(1)->nullable());
}
