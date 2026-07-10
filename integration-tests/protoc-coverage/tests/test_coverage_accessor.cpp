// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-1 companion: compile AND execute the generated RBA C++ accessor for the
// coverage fixture. Build an Arrow RecordBatch matching CompositeCoverage (from
// the SAME populated row the harness test uses, via ToArrowRow), construct the
// generated CompositeCoverageAccessor, and read representative scalar, optional,
// struct, repeated, map and nested-list values.
//
// This is a read-only guard over the RBA emitter — it does not reshape RBA.
// Nested-list reads go no deeper than the depth-2/3 cap (locked decision #3).

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <fletcher/pubsub/owned_schema.hpp>

#include "coverage.fletcher.accessor.pb.h"
#include "coverage.fletcher.arrow.pb.h"
#include "coverage.fletcher.pb.h"
#include "coverage_fixture.hpp"

using namespace fletcher;
namespace gen = fletcher_gen::integration::coverage;
namespace fx = coverage_fixture;

namespace {

std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

// Build a 1-row RecordBatch matching CompositeCoverage from the generated row:
// ToArrowRow yields one Arrow scalar per schema field (proven type-consistent
// by arrow-bridge's nullability_consistency test), and each is expanded to a
// length-1 column. This gives a schema-conformant batch without hand-building
// every nested/ map/ list column.
std::shared_ptr<arrow::RecordBatch> MakeCompositeBatch(const gen::CompositeCoverage& row) {
    auto schema = ImportNano(gen::CompositeCoverageSchema());
    if (!schema) return nullptr;

    const ArrowRow scalars = ToArrowRow(row);
    if (static_cast<int>(scalars.size()) != schema->num_fields()) {
        ADD_FAILURE() << "ToArrowRow produced " << scalars.size() << " scalars, schema has "
                      << schema->num_fields() << " fields";
        return nullptr;
    }

    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(scalars.size());
    for (size_t i = 0; i < scalars.size(); ++i) {
        auto arr = arrow::MakeArrayFromScalar(*scalars[i], 1);
        if (!arr.ok()) {
            ADD_FAILURE() << "MakeArrayFromScalar field " << i << ": " << arr.status().ToString();
            return nullptr;
        }
        columns.push_back(*arr);
    }
    return arrow::RecordBatch::Make(schema, 1, columns);
}

}  // namespace

TEST(CoverageHarness, GeneratedAccessorCppCompilesAndReads) {
    // Build the batch via MakeArrayFromScalar (see MakeCompositeBatch). That
    // path renders an UNSET optional message as a null struct WITH null
    // children, which the generated accessor rejects for the non-nullable inner
    // fields. The shared fixture leaves optional_branch unset (the harness test
    // covers that null-optional path at the edge-row level), so set it here to
    // keep the batch valid; the accessor reads the optional as SET.
    gen::CompositeCoverage row = fx::MakeComposite();
    gen::Branch optional_branch;
    optional_branch.set_leaf(fx::MakeLeaf(20, "ob", fx::kTopLevelStatusOk));
    // Every nested optional message must be set too: MakeArrayFromScalar renders
    // an unset optional message as a null struct with null children, which the
    // accessor rejects for the non-nullable inner Leaf.id.
    optional_branch.set_optional_leaf(fx::MakeLeaf(21, "ob-opt", fx::kTopLevelStatusWarn));
    row.set_optional_branch(optional_branch);

    const auto batch = MakeCompositeBatch(row);
    ASSERT_NE(batch, nullptr);

    auto acc = gen::CompositeCoverageAccessor::Make(batch);
    ASSERT_TRUE(acc.ok()) << acc.status().ToString();
    const auto& a = *acc;
    ASSERT_EQ(a.num_rows(), 1);

    // scalar through a nested struct RowView (+ enum-as-int32)
    EXPECT_EQ(a.scalars(0).int32_value(), fx::kInt32);
    EXPECT_EQ(a.scalars(0).string_value(), fx::StringValue());
    EXPECT_EQ(a.scalars(0).status(), fx::kTopLevelStatusWarn);

    // optional structs (both set here) read through as present RowViews
    ASSERT_TRUE(a.optional_scalars(0).has_value());
    EXPECT_EQ(a.optional_scalars(0)->int32_value(), fx::kInt32);
    ASSERT_TRUE(a.optional_branch(0).has_value());
    EXPECT_EQ(a.optional_branch(0)->leaf().id(), 20);

    // non-nullable struct chain
    EXPECT_EQ(a.branch(0).leaf().id(), 1);
    EXPECT_EQ(a.branch(0).leaf().label(), "root");

    // repeated scalar
    const auto rs = a.repeated_scalar(0);
    ASSERT_EQ(rs.size(), 3);
    EXPECT_EQ(rs[2], 30);

    // repeated struct
    const auto rst = a.repeated_struct(0);
    ASSERT_EQ(rst.size(), 2);
    ASSERT_TRUE(rst[0].has_value());
    EXPECT_EQ(rst[0]->id(), 5);

    // scalar map
    const auto ms = a.map_scalar(0);
    EXPECT_EQ(ms.size(), 2);

    // message map
    const auto mst = a.map_struct(0);
    ASSERT_EQ(mst.size(), 1);
    EXPECT_EQ(mst.key(0), std::string_view("k"));
    ASSERT_TRUE(mst.value(0).has_value());
    EXPECT_EQ(mst.value(0)->id(), 7);

    // message-level flatten (list<struct>)
    const auto fsl = a.flattened_struct_list(0);
    ASSERT_EQ(fsl.size(), 1);
    ASSERT_TRUE(fsl[0].has_value());
    EXPECT_EQ(fsl[0]->id(), 8);

    // struct-leaf nested list depth 2 (read no deeper than the RBA cap)
    const auto nsl = a.nested_struct_lists(0);
    ASSERT_EQ(nsl.size(), 2);
    ASSERT_TRUE(nsl[0].has_value());
    ASSERT_EQ(nsl[0]->size(), 1);
    ASSERT_TRUE((*nsl[0])[0].has_value());
    EXPECT_EQ((*nsl[0])[0]->id(), 9);

    // field-level flatten (inlined x,y) via a nested RowView
    EXPECT_DOUBLE_EQ(a.field_flattened_position(0).x(), 3.75);
    EXPECT_DOUBLE_EQ(a.field_flattened_position(0).y(), 4.5);
}
