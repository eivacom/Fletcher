// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-10 §3d forcing test: Nesting.ListOfListOfScalarRoundTrips.
//
// Proves the scalar-leaf nested-list shapes (parked in coverage_future.proto by
// GIR-1) are now faithful end to end on the ScalarNestedCoverage fixture:
//
//   * generated edge encode -> decode -> per-field equality
//   * ToArrowRow -> Codec::EncodeRow -> DecodeRow -> `<Class>View` -> reconstruct
//   * the Arrow schema for `nested_scalar_lists` is TRULY list<list<int32>>
//     (not collapsed to list<int32>), and the C++ storage is the nested
//     std::vector<std::vector<int32_t>> (the set_* signature pins this at compile
//     time), never a flat vector.
//
// Before GIR-10, `repeated ScalarListWrapper` COLLAPSED to a flat list<int32> in
// the IR (the caller's `repeated` level was dropped) and the depth-2/3 scalar
// leaves were dropped from the schema/TS entirely — so this round trip could not
// even be expressed. The bug class is (b) "unfaithful parked shape" (design §3).
//
// Nesting.SchemaEvolutionNegative* (§3d) proves the positional codec DETECTS a
// v1/v2 schema mismatch (rejects), rather than silently mis-decoding.
//
// Decode lifetime: DecodeRow may borrow the source buffer zero-copy, so every
// EncodedRow is a NAMED local that outlives the view built from it.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "coverage_scalar_nested.fletcher.arrow.pb.h"
#include "coverage_scalar_nested.fletcher.pb.h"
#include "coverage_test_helpers.hpp"

namespace gen = fletcher_gen::integration::coverage::scalar_nested;

namespace {

using V1 = std::vector<int32_t>;
using V2 = std::vector<std::vector<int32_t>>;
using V3 = std::vector<std::vector<std::vector<int32_t>>>;

const V1 kFlat = {5, 6, 7};
const V1 kOptFlat = {8, 9};
// Includes an empty inner list to exercise the count=0 element path.
const V2 kNested = {{1, 2, 3}, {}, {4}};
// Includes an empty middle ring to exercise the count=0 mid path.
const V3 kDepth3 = {{{1}, {2, 3}}, {{}, {4, 5, 6}}};

gen::ScalarNestedCoverage MakeFixture() {
    gen::ScalarNestedCoverage m;
    m.set_flattened_scalar_list(kFlat);
    m.set_optional_flattened_scalar_list(kOptFlat);
    m.set_nested_scalar_lists(kNested);
    m.set_depth3_scalar_lists(kDepth3);
    return m;
}

// Rebuild nested vectors from the generated Arrow view getters (ArrowScalarList /
// ArrowNestedScalarList / ArrowNestedScalarList2).
template <typename ScalarList>
V1 RebuildV1(const ScalarList& sl) {
    V1 out;
    for (int64_t i = 0; i < sl.size(); ++i) out.push_back(sl[i]);
    return out;
}

template <typename NestedList>
V2 RebuildV2(const NestedList& nl) {
    V2 out;
    for (int64_t i = 0; i < nl.size(); ++i) out.push_back(RebuildV1(nl[i]));
    return out;
}

template <typename NestedList2>
V3 RebuildV3(const NestedList2& nl) {
    V3 out;
    for (int64_t i = 0; i < nl.size(); ++i) out.push_back(RebuildV2(nl[i]));
    return out;
}

}  // namespace

TEST(Nesting, ListOfListOfScalarRoundTrips) {
    const gen::ScalarNestedCoverage original = MakeFixture();

    // ---- 1. generated edge encode -> decode -> per-field equality -----------
    const fletcher::EncodedRow edge_bytes = original.Encode();
    const gen::ScalarNestedCoverage edge_decoded(edge_bytes);
    EXPECT_EQ(edge_decoded.flattened_scalar_list(), kFlat);
    EXPECT_EQ(edge_decoded.optional_flattened_scalar_list(), kOptFlat);
    // The primary forcing shape: List<List<int32>> survives without collapse.
    EXPECT_EQ(edge_decoded.nested_scalar_lists(), kNested);
    EXPECT_EQ(edge_decoded.depth3_scalar_lists(), kDepth3);
    EXPECT_EQ(edge_decoded.Encode(), edge_bytes) << "edge encode->decode not a fixpoint";

    // ---- 2. the Arrow schema is TRULY list<list<int32>> (not collapsed) -----
    auto schema = coverage_test::ImportNano(gen::ScalarNestedCoverageSchema());
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(schema->num_fields(), 4);

    // field 2 (0-based) = nested_scalar_lists.
    {
        auto t = schema->field(2)->type();
        ASSERT_EQ(t->id(), arrow::Type::LIST) << "nested_scalar_lists is not a list";
        auto inner = static_cast<const arrow::ListType&>(*t).value_type();
        ASSERT_EQ(inner->id(), arrow::Type::LIST)
            << "nested_scalar_lists COLLAPSED to list<int32> — the inner list level was lost";
        auto leaf = static_cast<const arrow::ListType&>(*inner).value_type();
        EXPECT_EQ(leaf->id(), arrow::Type::INT32);
    }
    // field 3 = depth3_scalar_lists = list<list<list<int32>>>.
    {
        auto t = schema->field(3)->type();
        ASSERT_EQ(t->id(), arrow::Type::LIST);
        auto l2 = static_cast<const arrow::ListType&>(*t).value_type();
        ASSERT_EQ(l2->id(), arrow::Type::LIST);
        auto l3 = static_cast<const arrow::ListType&>(*l2).value_type();
        ASSERT_EQ(l3->id(), arrow::Type::LIST);
        auto leaf = static_cast<const arrow::ListType&>(*l3).value_type();
        EXPECT_EQ(leaf->id(), arrow::Type::INT32);
    }

    // ---- 3. ToArrowRow -> Codec -> View reconstruction ----------------------
    const fletcher::ArrowRow row = ToArrowRow(original);
    ASSERT_EQ(static_cast<int>(row.size()), schema->num_fields());

    fletcher::Codec codec(schema);
    const fletcher::EncodedRow codec_bytes = codec.EncodeRow(row);
    ASSERT_FALSE(codec_bytes.empty());
    fletcher::ArrowRow decoded = codec.DecodeRow(codec_bytes);

    const gen::ScalarNestedCoverageView view(std::move(decoded));
    EXPECT_EQ(RebuildV1(view.flattened_scalar_list()), kFlat);
    EXPECT_EQ(RebuildV1(view.optional_flattened_scalar_list()), kOptFlat);
    EXPECT_EQ(RebuildV2(view.nested_scalar_lists()), kNested);
    EXPECT_EQ(RebuildV3(view.depth3_scalar_lists()), kDepth3);

    // Explicit shape read-backs on the view: the second list level is a real
    // nested list, not a flattened span.
    const auto nsl = view.nested_scalar_lists();
    ASSERT_EQ(nsl.size(), 3);
    ASSERT_EQ(nsl[0].size(), 3);
    ASSERT_EQ(nsl[1].size(), 0);  // empty inner list preserved
    ASSERT_EQ(nsl[2].size(), 1);
    EXPECT_EQ(nsl[0][2], 3);
    EXPECT_EQ(nsl[2][0], 4);
}

// ---------------------------------------------------------------------------
// Schema-evolution negative tests (§3d). The positional codec is schema-driven
// with no field tags, so a v1/v2 mismatch must be DETECTED (a decode throw),
// never silently mis-decoded into adjacent fields.
// ---------------------------------------------------------------------------

namespace {

// v1: an earlier ScalarNestedCoverage that has ONLY the two flat list<int32>
// fields (before nested_scalar_lists / depth3_scalar_lists were added).
std::shared_ptr<arrow::Schema> SchemaV1() {
    return arrow::schema({
        arrow::field("flattened_scalar_list", arrow::list(arrow::int32())),
        arrow::field("optional_flattened_scalar_list", arrow::list(arrow::int32())),
    });
}

// v2: the full schema with the two added nested-list fields.
std::shared_ptr<arrow::Schema> SchemaV2() {
    return arrow::schema({
        arrow::field("flattened_scalar_list", arrow::list(arrow::int32())),
        arrow::field("optional_flattened_scalar_list", arrow::list(arrow::int32())),
        arrow::field("nested_scalar_lists", arrow::list(arrow::list(arrow::int32()))),
        arrow::field("depth3_scalar_lists", arrow::list(arrow::list(arrow::list(arrow::int32())))),
    });
}

std::shared_ptr<arrow::Scalar> ListOf(std::vector<int32_t> vals) {
    arrow::Int32Builder b;
    for (int32_t v : vals) (void)b.Append(v);
    return std::make_shared<arrow::ListScalar>(b.Finish().ValueOrDie());
}

}  // namespace

TEST(Nesting, SchemaEvolutionNegativeV1DecodedAsV2) {
    // A v1 row (2 fields) fed to a v2 decoder (4 fields) must be REJECTED: after
    // the two lists are consumed, the decoder reaches for nested_scalar_lists'
    // count past the end of the buffer -> underrun throw, not garbage.
    fletcher::Codec v1(SchemaV1());
    const fletcher::EncodedRow v1_bytes = v1.EncodeRow({ListOf({1, 2}), ListOf({3})});

    fletcher::Codec v2(SchemaV2());
    EXPECT_THROW(v2.DecodeRow(v1_bytes), std::invalid_argument);
}

TEST(Nesting, SchemaEvolutionNegativeV2DecodedAsV1) {
    // A v2 row (4 fields) fed to a v1 decoder (2 fields) must be REJECTED: the two
    // trailing nested-list fields leave unconsumed bytes -> "buffer not fully
    // consumed" throw, not a silently truncated decode.
    fletcher::Codec v2(SchemaV2());

    arrow::Int32Builder ib;
    (void)ib.Append(1);
    (void)ib.Append(2);
    auto inner = std::make_shared<arrow::ListScalar>(ib.Finish().ValueOrDie());
    arrow::ListBuilder outer(arrow::default_memory_pool(), std::make_shared<arrow::Int32Builder>());
    (void)outer.Append();
    auto* child = static_cast<arrow::Int32Builder*>(outer.value_builder());
    (void)child->Append(7);
    auto nested_arr = outer.Finish().ValueOrDie();
    auto nested_scalar = std::make_shared<arrow::ListScalar>(nested_arr);

    arrow::ListBuilder d3_outer(
        arrow::default_memory_pool(),
        std::make_shared<arrow::ListBuilder>(arrow::default_memory_pool(),
                                             std::make_shared<arrow::Int32Builder>()));
    auto d3_arr = d3_outer.Finish().ValueOrDie();
    auto d3_scalar = std::make_shared<arrow::ListScalar>(d3_arr);

    const fletcher::EncodedRow v2_bytes =
        v2.EncodeRow({ListOf({1, 2}), ListOf({3}), nested_scalar, d3_scalar});

    fletcher::Codec v1(SchemaV1());
    EXPECT_THROW(v1.DecodeRow(v2_bytes), std::invalid_argument);
}
