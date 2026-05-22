// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// flatten.proto — Tests for the (fletcher.flatten) option:
//
// - Case A: scalar wrapper (message-level flatten)
// - Case B: struct composition (field-level flatten)
// - Case C: chained message-level flatten (geometry-style nesting)
// - Case D: multi-field message with flatten (ignored + warning)
// - nullable flatten wrapper
// - repeated flatten wrapper

#include "flatten.fletcher.pb.h"

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

// ---- Case A: scalar wrapper (message-level flatten) --------------------

TEST(FlattenTest, ScalarWrapperBecomesScalar) {
    auto schema = ImportNano(fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_TRUE(schema);

    // Field 0: "reading" — Temperature is flattened, so it becomes Float32
    // (not Struct<celsius: Float32>).
    EXPECT_EQ(schema->field(0)->name(), "reading");
    EXPECT_EQ(schema->field(0)->type()->id(), arrow::Type::FLOAT);
    EXPECT_FALSE(schema->field(0)->nullable());
}

TEST(FlattenTest, ScalarWrapperRoundtrip) {
    fletcher_gen::integration::FlattenTestRow r;
    r.set_reading(36.6f);

    // Fill remaining required fields with defaults.
    r.set_position(fletcher_gen::integration::Point{});
    r.set_shape({});
    r.set_bad(fletcher_gen::integration::BadFlatten{});

    auto scalars = RoundTrip(r.Encode(),
                             fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_GE(scalars.size(), 1u);

    auto* temp = dynamic_cast<arrow::FloatScalar*>(scalars[0].get());
    ASSERT_NE(temp, nullptr);
    EXPECT_FLOAT_EQ(temp->value, 36.6f);
}

// ---- Case B: struct composition (field-level flatten) ------------------

TEST(FlattenTest, FieldFlattenRoundtrip) {
    fletcher_gen::integration::Point pt;
    pt.set_x(10.5).set_y(20.5);

    fletcher_gen::integration::FlattenTestRow r;
    r.set_reading(0.0f);
    r.set_position(pt);
    r.set_shape({});
    r.set_bad(fletcher_gen::integration::BadFlatten{});

    auto scalars = RoundTrip(r.Encode(),
                             fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_GE(scalars.size(), 2u);

    auto* pos = dynamic_cast<arrow::StructScalar*>(scalars[1].get());
    ASSERT_NE(pos, nullptr);
    EXPECT_TRUE(pos->is_valid);

    fletcher_gen::integration::FlattenTestRow decoded(r.Encode());
    EXPECT_DOUBLE_EQ(decoded.position().x(), 10.5);
    EXPECT_DOUBLE_EQ(decoded.position().y(), 20.5);
}

TEST(FlattenTest, FieldFlattenInlinesFields) {
    auto schema = ImportNano(fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_TRUE(schema);

    // Field 1: "position" — Point with field-level flatten on Coord inlines
    // x and y directly, so Point becomes Struct<x: f64, y: f64>.
    EXPECT_EQ(schema->field(1)->name(), "position");
    EXPECT_EQ(schema->field(1)->type()->id(), arrow::Type::STRUCT);

    auto struct_type = std::dynamic_pointer_cast<arrow::StructType>(
        schema->field(1)->type());
    ASSERT_TRUE(struct_type);
    ASSERT_EQ(struct_type->num_fields(), 2);
    EXPECT_EQ(struct_type->field(0)->name(), "x");
    EXPECT_EQ(struct_type->field(0)->type()->id(), arrow::Type::DOUBLE);
    EXPECT_EQ(struct_type->field(1)->name(), "y");
    EXPECT_EQ(struct_type->field(1)->type()->id(), arrow::Type::DOUBLE);
}

// ---- Case C: chained flatten (geometry nesting) ------------------------

TEST(FlattenTest, ChainedFlattenProducesNestedLists) {
    auto schema = ImportNano(fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_TRUE(schema);

    // Field 2: "shape" — Polygon is flattened, repeated Ring (also flattened)
    // with repeated Coord.  Result: List<List<Struct<x,y>>>.
    EXPECT_EQ(schema->field(2)->name(), "shape");

    auto outer_list = std::dynamic_pointer_cast<arrow::ListType>(
        schema->field(2)->type());
    ASSERT_TRUE(outer_list) << "Expected outer List, got "
                            << schema->field(2)->type()->ToString();

    auto inner_list = std::dynamic_pointer_cast<arrow::ListType>(
        outer_list->value_type());
    ASSERT_TRUE(inner_list) << "Expected inner List, got "
                            << outer_list->value_type()->ToString();

    auto coord_struct = std::dynamic_pointer_cast<arrow::StructType>(
        inner_list->value_type());
    ASSERT_TRUE(coord_struct) << "Expected leaf Struct, got "
                              << inner_list->value_type()->ToString();
    ASSERT_EQ(coord_struct->num_fields(), 2);
    EXPECT_EQ(coord_struct->field(0)->name(), "x");
    EXPECT_EQ(coord_struct->field(1)->name(), "y");
}

TEST(FlattenTest, ChainedFlattenRoundtrip) {
    fletcher_gen::integration::Coord c1, c2, c3;
    c1.set_x(0.0).set_y(0.0);
    c2.set_x(1.0).set_y(0.0);
    c3.set_x(0.0).set_y(1.0);

    fletcher_gen::integration::FlattenTestRow r;
    r.set_reading(0.0f);
    r.set_position(fletcher_gen::integration::Point{});
    r.set_shape({{c1, c2, c3}});
    r.set_bad(fletcher_gen::integration::BadFlatten{});

    auto scalars = RoundTrip(r.Encode(),
                             fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_GE(scalars.size(), 3u);

    auto* outer = dynamic_cast<arrow::ListScalar*>(scalars[2].get());
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->value->length(), 1);

    fletcher_gen::integration::FlattenTestRow decoded(r.Encode());
    ASSERT_EQ(decoded.shape().size(), 1u);
    ASSERT_EQ(decoded.shape()[0].size(), 3u);
    EXPECT_DOUBLE_EQ(decoded.shape()[0][0].x(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.shape()[0][0].y(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.shape()[0][2].x(), 0.0);
    EXPECT_DOUBLE_EQ(decoded.shape()[0][2].y(), 1.0);
}

// ---- Case D: multi-field flatten (ignored) -----------------------------

TEST(FlattenTest, MultifieldFlattenIgnored) {
    auto schema = ImportNano(fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_TRUE(schema);

    // Field 4: "bad" — BadFlatten has 2 fields so flatten is ignored,
    // producing a normal Struct<x: f64, y: f64>.
    EXPECT_EQ(schema->field(4)->name(), "bad");
    EXPECT_EQ(schema->field(4)->type()->id(), arrow::Type::STRUCT);

    auto struct_type = std::dynamic_pointer_cast<arrow::StructType>(
        schema->field(4)->type());
    ASSERT_TRUE(struct_type);
    ASSERT_EQ(struct_type->num_fields(), 2);
    EXPECT_EQ(struct_type->field(0)->name(), "x");
    EXPECT_EQ(struct_type->field(1)->name(), "y");
}

// ---- Repeated flatten wrapper ------------------------------------------

TEST(FlattenTest, RepeatedFlattenWrapperBecomesListScalar) {
    auto schema = ImportNano(fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_TRUE(schema);

    // Field 3: "temps" — repeated Temperature, where Temperature is flattened.
    // Result: List<Float32> (not List<Struct<celsius>>).
    EXPECT_EQ(schema->field(3)->name(), "temps");

    auto list_type = std::dynamic_pointer_cast<arrow::ListType>(
        schema->field(3)->type());
    ASSERT_TRUE(list_type) << "Expected List, got "
                           << schema->field(3)->type()->ToString();
    EXPECT_EQ(list_type->value_type()->id(), arrow::Type::FLOAT);
}

// ---- Optional flatten wrapper ------------------------------------------

TEST(FlattenTest, OptionalFlattenWrapperIsNullable) {
    auto schema = ImportNano(fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_TRUE(schema);

    // Field 5: "opt_temp" — optional Temperature, flattened to nullable Float32.
    EXPECT_EQ(schema->field(5)->name(), "opt_temp");
    EXPECT_EQ(schema->field(5)->type()->id(), arrow::Type::FLOAT);
    EXPECT_TRUE(schema->field(5)->nullable());
}

TEST(FlattenTest, OptionalFlattenNullWhenNotSet) {
    fletcher_gen::integration::FlattenTestRow r;
    r.set_reading(0.0f);
    r.set_position(fletcher_gen::integration::Point{});
    r.set_shape({});
    r.set_bad(fletcher_gen::integration::BadFlatten{});

    auto scalars = RoundTrip(r.Encode(),
                             fletcher_gen::integration::FlattenTestRowSchema());
    ASSERT_GE(scalars.size(), 6u);
    EXPECT_FALSE(scalars[5]->is_valid);
}
