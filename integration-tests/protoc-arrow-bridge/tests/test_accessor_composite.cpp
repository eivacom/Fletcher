// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// RBA-4a forcing test — the struct + repeated-scalar + repeated-struct slice of
// AccessorTest.CompositeColumnsReadColumnOriented.
//
// Builds an arrow::RecordBatch / StructArray for accessor_composite.proto
// (CompositeRow) and proves:
//   - STRUCT, cross-file, >= 2 levels: the deep chain a.outer(row).inner().leaf()
//     reads through composed RowViews and a cross-file InnerAccessor/OuterAccessor;
//   - nullable 1:1 struct: a null `maybe_outer` row -> std::nullopt (B2);
//   - non-nullable struct with a runtime null element -> Make() error (D-RBA-4,
//     recursed into the composite null_count gate);
//   - REPEATED_SCALAR: span size / empty list / element reads / span.is_null(j);
//   - REPEATED_STRUCT: span[j]==std::nullopt for a null struct element
//     (element-level no-read-through-null via the inner accessor is_null);
//   - name tolerance on the nested/cross-file struct field (rename Arrow fields,
//     keep order/types -> Make still succeeds);
//   - Make(StructArray) reads identically and is_null(row) reflects struct validity.
//
// Maps and nested lists are RBA-4b and are intentionally NOT exercised here.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "accessor_composite.fletcher.accessor.pb.h"

namespace {

using fletcher_gen::integration::CompositeRowAccessor;

// inner = struct<leaf:int32 (non-nullable)>
std::shared_ptr<arrow::DataType> InnerType() {
    return arrow::struct_({arrow::field("leaf", arrow::int32(), /*nullable=*/false)});
}

// outer = struct<inner: inner (non-nullable)>
std::shared_ptr<arrow::DataType> OuterType() {
    return arrow::struct_({arrow::field("inner", InnerType(), /*nullable=*/false)});
}

// Build an Inner StructArray of `leaf` values (no struct-level nulls).
std::shared_ptr<arrow::StructArray> MakeInnerArray(const std::vector<int32_t>& leaves) {
    arrow::Int32Builder lb;
    EXPECT_TRUE(lb.AppendValues(leaves).ok());
    std::shared_ptr<arrow::Array> leaf_arr;
    EXPECT_TRUE(lb.Finish(&leaf_arr).ok());
    auto r = arrow::StructArray::Make({leaf_arr}, {arrow::field("leaf", arrow::int32(), false)});
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::StructArray>(*r);
}

// Build an Outer StructArray wrapping the given Inner array. `outer_valid` (if
// non-empty) supplies a struct-level validity bitmap for the Outer elements.
// Build a packed validity bitmap buffer from a per-element valid mask, or
// nullptr when the mask is empty (all valid).
std::shared_ptr<arrow::Buffer> MakeValidityBuffer(const std::vector<bool>& valid) {
    if (valid.empty()) return nullptr;
    arrow::TypedBufferBuilder<bool> bb;
    for (bool v : valid) EXPECT_TRUE(bb.Append(v).ok());
    std::shared_ptr<arrow::Buffer> buf;
    EXPECT_TRUE(bb.Finish(&buf).ok());
    return buf;
}

std::shared_ptr<arrow::StructArray> MakeOuterArray(const std::shared_ptr<arrow::StructArray>& inner,
                                                   const std::vector<bool>& outer_valid = {}) {
    auto r = arrow::StructArray::Make({inner}, {arrow::field("inner", InnerType(), false)},
                                      MakeValidityBuffer(outer_valid));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::StructArray>(*r);
}

// list<double> from per-row offsets and a (possibly null-bearing) values child.
std::shared_ptr<arrow::ListArray> MakeDoubleList(
    const std::vector<int32_t>& offsets,
    const std::vector<std::optional<double>>& values) {
    arrow::DoubleBuilder vb;
    for (const auto& v : values) {
        if (v.has_value()) {
            EXPECT_TRUE(vb.Append(*v).ok());
        } else {
            EXPECT_TRUE(vb.AppendNull().ok());
        }
    }
    std::shared_ptr<arrow::Array> vals;
    EXPECT_TRUE(vb.Finish(&vals).ok());

    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off;
    EXPECT_TRUE(ob.Finish(&off).ok());

    auto r = arrow::ListArray::FromArrays(*off, *vals);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::ListArray>(*r);
}

// list<struct<leaf:int32>> from per-row offsets and an Inner values child that
// may carry struct-level nulls.
std::shared_ptr<arrow::ListArray> MakeInnerList(const std::vector<int32_t>& offsets,
                                                const std::shared_ptr<arrow::StructArray>& values) {
    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off;
    EXPECT_TRUE(ob.Finish(&off).ok());

    auto r = arrow::ListArray::FromArrays(*off, *values);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::ListArray>(*r);
}

// Inner array with one struct-level null element (index `null_at`).
std::shared_ptr<arrow::StructArray> MakeInnerArrayWithNull(const std::vector<int32_t>& leaves,
                                                           int null_at) {
    arrow::Int32Builder lb;
    EXPECT_TRUE(lb.AppendValues(leaves).ok());
    std::shared_ptr<arrow::Array> leaf_arr;
    EXPECT_TRUE(lb.Finish(&leaf_arr).ok());

    arrow::TypedBufferBuilder<bool> bb;
    for (int i = 0; i < static_cast<int>(leaves.size()); ++i)
        EXPECT_TRUE(bb.Append(i != null_at).ok());
    std::shared_ptr<arrow::Buffer> validity;
    EXPECT_TRUE(bb.Finish(&validity).ok());

    auto r = arrow::StructArray::Make({leaf_arr}, {arrow::field("leaf", arrow::int32(), false)},
                                      validity);
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::StructArray>(*r);
}

// list<double> with NULL list rows (row-level validity), distinguishing a null
// list row from an empty list. `row_valid` is one flag per output row.
std::shared_ptr<arrow::ListArray> MakeNullableDoubleList(
    const std::vector<int32_t>& offsets, const std::vector<std::optional<double>>& values,
    const std::vector<bool>& row_valid) {
    arrow::DoubleBuilder vb;
    for (const auto& v : values) {
        if (v.has_value()) {
            EXPECT_TRUE(vb.Append(*v).ok());
        } else {
            EXPECT_TRUE(vb.AppendNull().ok());
        }
    }
    std::shared_ptr<arrow::Array> vals;
    EXPECT_TRUE(vb.Finish(&vals).ok());

    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off;
    EXPECT_TRUE(ob.Finish(&off).ok());

    auto r = arrow::ListArray::FromArrays(*off, *vals, arrow::default_memory_pool(),
                                          MakeValidityBuffer(row_valid));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::ListArray>(*r);
}

// list<struct<leaf:int32>> with NULL list rows (row-level validity).
std::shared_ptr<arrow::ListArray> MakeNullableInnerList(
    const std::vector<int32_t>& offsets, const std::shared_ptr<arrow::StructArray>& values,
    const std::vector<bool>& row_valid) {
    arrow::Int32Builder ob;
    EXPECT_TRUE(ob.AppendValues(offsets).ok());
    std::shared_ptr<arrow::Array> off;
    EXPECT_TRUE(ob.Finish(&off).ok());

    auto r = arrow::ListArray::FromArrays(*off, *values, arrow::default_memory_pool(),
                                          MakeValidityBuffer(row_valid));
    EXPECT_TRUE(r.ok()) << r.status().ToString();
    return std::static_pointer_cast<arrow::ListArray>(*r);
}

constexpr int64_t kNumRows = 3;

// Schema for CompositeRow: outer (non-nullable struct), maybe_outer (nullable
// struct), readings (list<double>), track (list<struct<leaf>>), opt_readings
// (row-nullable list<double>), opt_track (row-nullable list<struct<leaf>>).
std::shared_ptr<arrow::Schema> MakeSchema() {
    return arrow::schema({
        arrow::field("outer", OuterType(), /*nullable=*/false),
        arrow::field("maybe_outer", OuterType(), /*nullable=*/true),
        arrow::field("readings", arrow::list(arrow::field("item", arrow::float64())),
                     /*nullable=*/false),
        arrow::field("track",
                     arrow::list(arrow::field("item", InnerType())),
                     /*nullable=*/false),
        arrow::field("opt_readings", arrow::list(arrow::field("item", arrow::float64())),
                     /*nullable=*/true),
        arrow::field("opt_track", arrow::list(arrow::field("item", InnerType())),
                     /*nullable=*/true),
    });
}

// Build the canonical 3-row happy-path batch.
//   row 0: outer.inner.leaf=10, maybe_outer=present(leaf=110), readings=[1.5,2.5],
//          track=[leaf=21, leaf=22]
//   row 1: outer.inner.leaf=20, maybe_outer=NULL,             readings=[] (empty),
//          track=[leaf=23 (NULL element)]
//   row 2: outer.inner.leaf=30, maybe_outer=present(leaf=130), readings=[3.5 (NULL)],
//          track=[]
struct Cols {
    std::shared_ptr<arrow::Array> outer, maybe_outer, readings, track, opt_readings, opt_track;
    arrow::ArrayVector AsVector() const {
        return {outer, maybe_outer, readings, track, opt_readings, opt_track};
    }
};

Cols MakeFixtureCols() {
    Cols c;
    c.outer = MakeOuterArray(MakeInnerArray({10, 20, 30}));
    c.maybe_outer =
        MakeOuterArray(MakeInnerArray({110, 0, 130}), /*outer_valid=*/{true, false, true});

    // readings: row0=[1.5,2.5], row1=[], row2=[NULL]
    c.readings = MakeDoubleList(/*offsets=*/{0, 2, 2, 3},
                                /*values=*/{1.5, 2.5, std::nullopt});

    // track: row0=[21,22], row1=[NULL element], row2=[]
    auto track_vals = MakeInnerArrayWithNull({21, 22, 0}, /*null_at=*/2);
    c.track = MakeInnerList(/*offsets=*/{0, 2, 3, 3}, track_vals);

    // opt_readings (row-nullable): row0=[10.0,20.0], row1=NULL list, row2=[] empty.
    c.opt_readings = MakeNullableDoubleList(/*offsets=*/{0, 2, 2, 2},
                                            /*values=*/{10.0, 20.0},
                                            /*row_valid=*/{true, false, true});

    // opt_track (row-nullable): row0=[leaf=51], row1=NULL list, row2=[] empty.
    auto opt_track_vals = MakeInnerArray({51});
    c.opt_track = MakeNullableInnerList(/*offsets=*/{0, 1, 1, 1}, opt_track_vals,
                                        /*row_valid=*/{true, false, true});
    return c;
}

std::shared_ptr<arrow::RecordBatch> MakeFixtureBatch() {
    return arrow::RecordBatch::Make(MakeSchema(), kNumRows, MakeFixtureCols().AsVector());
}

void CheckHappyPath(const CompositeRowAccessor& a) {
    ASSERT_EQ(a.num_rows(), kNumRows);

    // STRUCT, cross-file, >= 2 levels: deep chain through composed RowViews.
    EXPECT_EQ(a.outer(0).inner().leaf(), 10);
    EXPECT_EQ(a.outer(1).inner().leaf(), 20);
    EXPECT_EQ(a.outer(2).inner().leaf(), 30);

    // nullable 1:1 struct: row 1 is null -> nullopt; rows 0/2 present.
    ASSERT_TRUE(a.maybe_outer(0).has_value());
    EXPECT_EQ(a.maybe_outer(0)->inner().leaf(), 110);
    EXPECT_FALSE(a.maybe_outer(1).has_value());
    ASSERT_TRUE(a.maybe_outer(2).has_value());
    EXPECT_EQ(a.maybe_outer(2)->inner().leaf(), 130);

    // REPEATED_SCALAR: size / element reads / empty list / per-element is_null.
    auto r0 = a.readings(0);
    ASSERT_EQ(r0.size(), 2);
    EXPECT_FALSE(r0.empty());
    EXPECT_DOUBLE_EQ(r0[0], 1.5);
    EXPECT_DOUBLE_EQ(r0[1], 2.5);
    EXPECT_FALSE(r0.is_null(0));

    auto r1 = a.readings(1);
    EXPECT_EQ(r1.size(), 0);
    EXPECT_TRUE(r1.empty());

    auto r2 = a.readings(2);
    ASSERT_EQ(r2.size(), 1);
    EXPECT_TRUE(r2.is_null(0));  // scalar element null via is_null(j), not optional

    // REPEATED_STRUCT: element reads + null struct element -> nullopt.
    auto t0 = a.track(0);
    ASSERT_EQ(t0.size(), 2);
    ASSERT_TRUE(t0[0].has_value());
    EXPECT_EQ(t0[0]->leaf(), 21);
    ASSERT_TRUE(t0[1].has_value());
    EXPECT_EQ(t0[1]->leaf(), 22);

    auto t1 = a.track(1);
    ASSERT_EQ(t1.size(), 1);
    EXPECT_FALSE(t1[0].has_value());  // null struct element -> nullopt (B2 per element)
    EXPECT_TRUE(t1.is_null(0));

    auto t2 = a.track(2);
    EXPECT_EQ(t2.size(), 0);
    EXPECT_TRUE(t2.empty());

    // ROW-NULLABLE REPEATED_SCALAR (optional flatten-wrapper): the getter returns
    // std::optional<ScalarSpan>. A null list row -> std::nullopt, DISTINCT from an
    // empty list (B2 no-read-through-null at the list-row level).
    ASSERT_TRUE(a.opt_readings(0).has_value());
    {
        auto s = *a.opt_readings(0);
        ASSERT_EQ(s.size(), 2);
        EXPECT_DOUBLE_EQ(s[0], 10.0);
        EXPECT_DOUBLE_EQ(s[1], 20.0);
    }
    EXPECT_FALSE(a.opt_readings(1).has_value());  // null list row -> nullopt
    ASSERT_TRUE(a.opt_readings(2).has_value());   // empty list is present, not null
    EXPECT_TRUE(a.opt_readings(2)->empty());
    EXPECT_EQ(a.opt_readings(2)->size(), 0);

    // ROW-NULLABLE REPEATED_STRUCT (optional flatten-wrapper): std::optional<
    // StructSpan>. null list row -> std::nullopt; empty list present.
    ASSERT_TRUE(a.opt_track(0).has_value());
    {
        auto s = *a.opt_track(0);
        ASSERT_EQ(s.size(), 1);
        ASSERT_TRUE(s[0].has_value());
        EXPECT_EQ(s[0]->leaf(), 51);
    }
    EXPECT_FALSE(a.opt_track(1).has_value());  // null list row -> nullopt
    ASSERT_TRUE(a.opt_track(2).has_value());   // empty list is present, not null
    EXPECT_TRUE(a.opt_track(2)->empty());

    // RowView forwards the optional-span getters too.
    EXPECT_FALSE(a.at(1).opt_readings().has_value());
    ASSERT_TRUE(a.at(0).opt_track().has_value());
    EXPECT_EQ((*a.at(0).opt_track())[0]->leaf(), 51);
}

}  // namespace

TEST(AccessorTest, CompositeColumnsReadColumnOriented) {
    // (1) Happy path through RecordBatch.
    {
        auto batch = MakeFixtureBatch();
        auto r = CompositeRowAccessor::Make(batch);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        CheckHappyPath(*r);
        // RecordBatch-sourced accessor: is_null(row) is always false (no struct
        // validity bitmap retained).
        EXPECT_FALSE(r->is_null(0));
        EXPECT_FALSE(r->is_null(1));
    }

    // (2) Make(StructArray) reads identically.
    {
        auto cols = MakeFixtureCols().AsVector();
        auto struct_res = arrow::StructArray::Make(cols, MakeSchema()->fields());
        ASSERT_TRUE(struct_res.ok()) << struct_res.status().ToString();
        auto sa = std::static_pointer_cast<arrow::StructArray>(*struct_res);
        auto r = CompositeRowAccessor::Make(sa);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        CheckHappyPath(*r);
        // Struct-sourced with no top-level null bitmap -> is_null(row) false.
        EXPECT_FALSE(r->is_null(0));
    }

    // (3) Name tolerance on the nested / cross-file struct field: rename every
    //     Arrow field (top-level + struct children + list items) while keeping
    //     order and types. Make() must still succeed and read identically.
    {
        auto inner_renamed = arrow::struct_({arrow::field("renamed_leaf", arrow::int32(), false)});
        auto outer_renamed = arrow::struct_({arrow::field("renamed_inner", inner_renamed, false)});
        auto schema = arrow::schema({
            arrow::field("col0", outer_renamed, false),
            arrow::field("col1", outer_renamed, true),
            arrow::field("col2", arrow::list(arrow::field("x", arrow::float64())), false),
            arrow::field("col3", arrow::list(arrow::field("x", inner_renamed)), false),
            arrow::field("col4", arrow::list(arrow::field("x", arrow::float64())), true),
            arrow::field("col5", arrow::list(arrow::field("x", inner_renamed)), true),
        });
        auto batch = arrow::RecordBatch::Make(schema, kNumRows, MakeFixtureCols().AsVector());
        auto r = CompositeRowAccessor::Make(batch);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        EXPECT_EQ(r->outer(0).inner().leaf(), 10);
        EXPECT_FALSE(r->opt_readings(1).has_value());  // null list row tolerant of rename
    }

    // (4) non-nullable struct with a runtime null element -> Make() error.
    //     The recursed null_count gate (D-RBA-4) must reject a null `outer`.
    {
        auto cols = MakeFixtureCols();
        cols.outer = MakeOuterArray(MakeInnerArray({10, 20, 30}),
                                    /*outer_valid=*/{true, false, true});  // row 1 null
        auto batch = arrow::RecordBatch::Make(MakeSchema(), kNumRows, cols.AsVector());
        auto r = CompositeRowAccessor::Make(batch);
        ASSERT_FALSE(r.ok());
        const std::string msg = r.status().message();
        EXPECT_NE(msg.find("non-nullable"), std::string::npos) << msg;
        EXPECT_NE(msg.find("outer"), std::string::npos) << msg;
    }

    // (5) Make never throws on a null source pointer.
    {
        std::shared_ptr<arrow::RecordBatch> null_batch;
        auto r = CompositeRowAccessor::Make(null_batch);
        EXPECT_FALSE(r.ok());
    }
}

TEST(AccessorTest, CompositeAccessorKeepsDataAliveAfterBatchDropped) {
    std::optional<CompositeRowAccessor> acc;
    {
        auto batch = MakeFixtureBatch();
        auto r = CompositeRowAccessor::Make(batch);
        ASSERT_TRUE(r.ok()) << r.status().ToString();
        acc.emplace(std::move(*r));
        batch.reset();
    }
    ASSERT_TRUE(acc.has_value());
    CheckHappyPath(*acc);
}
