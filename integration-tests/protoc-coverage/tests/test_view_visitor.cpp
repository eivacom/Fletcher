// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-6 forcing test: ViewVisitor.RoundTripsViaCodec.
//
// Round-trips a populated CompositeCoverage through the migrated Arrow surfaces:
//
//   message --ToArrowRow()--> ArrowRow
//           --Codec::EncodeRow--> EncodedRow
//           --Codec::DecodeRow--> ArrowRow
//           --CompositeCoverageView--> typed getters
//           --reconstruct--> CompositeCoverage
//           --Equals (Encode() byte identity)--> original
//
// Both the ToArrowRow() field emission and the `<Class>View` getters are the
// GIR-6 IR-visitor output (cpp_backend_view_visitor.cpp). The reconstruction
// reads EVERY field back through the generated view getters and re-encodes; the
// byte-identical Encode() comparison is the "Equals". Explicit read-backs of map
// keys/values and struct inner fields pin the map + nested-view surfaces on top.
//
// Decode lifetime: Codec::DecodeRow hands back scalars that may borrow the source
// buffer zero-copy, so the EncodedRow is a NAMED local that outlives the View.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <fletcher/arrow_bridge/codec.hpp>

#include "coverage.fletcher.arrow.pb.h"
#include "coverage.fletcher.pb.h"
#include "coverage_fixture.hpp"
#include "coverage_test_helpers.hpp"

namespace gen = fletcher_gen::integration::coverage;
namespace fx = coverage_fixture;

namespace {

// ---- reconstruct edge messages from generated view getters ----------------

gen::Leaf RebuildLeaf(const gen::LeafView& v) {
    gen::Leaf l;
    l.set_id(v.id());
    l.set_label(v.label());
    l.set_status(v.status());
    return l;
}

template <typename LeafList>
std::vector<gen::Leaf> RebuildLeaves(const LeafList& lv) {
    std::vector<gen::Leaf> out;
    for (int64_t i = 0; i < lv.size(); ++i) out.push_back(RebuildLeaf(lv[i]));
    return out;
}

gen::ScalarCoverage RebuildScalars(const gen::ScalarCoverageView& v) {
    gen::ScalarCoverage s;
    s.set_bool_value(v.bool_value());
    s.set_int32_value(v.int32_value());
    s.set_int64_value(v.int64_value());
    s.set_uint32_value(v.uint32_value());
    s.set_uint64_value(v.uint64_value());
    s.set_sint32_value(v.sint32_value());
    s.set_sint64_value(v.sint64_value());
    s.set_fixed32_value(v.fixed32_value());
    s.set_fixed64_value(v.fixed64_value());
    s.set_sfixed32_value(v.sfixed32_value());
    s.set_sfixed64_value(v.sfixed64_value());
    s.set_float_value(v.float_value());
    s.set_double_value(v.double_value());
    s.set_string_value(v.string_value());
    s.set_bytes_value(v.bytes_value());
    s.set_timestamp_value(v.timestamp_value());
    s.set_duration_value(v.duration_value());
    s.set_status(v.status());
    s.set_nested_status(v.nested_status());

    // optional primitives — set only when present (mirrors the null axis).
    if (v.optional_bool().has_value()) s.set_optional_bool(*v.optional_bool());
    if (v.optional_int32().has_value()) s.set_optional_int32(*v.optional_int32());
    if (v.optional_string().has_value()) s.set_optional_string(*v.optional_string());
    if (v.optional_bytes().has_value()) s.set_optional_bytes(*v.optional_bytes());

    // WKT wrapper scalars — nullable, set only when present.
    if (v.wrapped_bool().has_value()) s.set_wrapped_bool(*v.wrapped_bool());
    if (v.wrapped_int32().has_value()) s.set_wrapped_int32(*v.wrapped_int32());
    if (v.wrapped_int64().has_value()) s.set_wrapped_int64(*v.wrapped_int64());
    if (v.wrapped_uint32().has_value()) s.set_wrapped_uint32(*v.wrapped_uint32());
    if (v.wrapped_uint64().has_value()) s.set_wrapped_uint64(*v.wrapped_uint64());
    if (v.wrapped_float().has_value()) s.set_wrapped_float(*v.wrapped_float());
    if (v.wrapped_double().has_value()) s.set_wrapped_double(*v.wrapped_double());
    if (v.wrapped_string().has_value()) s.set_wrapped_string(*v.wrapped_string());
    if (v.wrapped_bytes().has_value()) s.set_wrapped_bytes(*v.wrapped_bytes());
    return s;
}

gen::Branch RebuildBranch(const gen::BranchView& v) {
    gen::Branch b;
    b.set_leaf(RebuildLeaf(v.leaf()));
    if (v.optional_leaf().has_value()) b.set_optional_leaf(RebuildLeaf(*v.optional_leaf()));
    b.set_leaves(RebuildLeaves(v.leaves()));
    return b;
}

gen::FlattenedPoint RebuildPoint(const gen::FlattenedPointView& v) {
    gen::FlattenedPoint p;
    p.set_x(v.x());
    p.set_y(v.y());
    return p;
}

gen::FieldFlattenedPosition RebuildPosition(const gen::FieldFlattenedPositionView& v) {
    gen::FieldFlattenedPosition p;
    p.set_x(v.x());
    p.set_y(v.y());
    return p;
}

gen::CompositeCoverage RebuildComposite(const gen::CompositeCoverageView& v) {
    gen::CompositeCoverage c;

    c.set_scalars(RebuildScalars(v.scalars()));
    if (v.optional_scalars().has_value()) c.set_optional_scalars(RebuildScalars(*v.optional_scalars()));

    c.set_branch(RebuildBranch(v.branch()));
    if (v.optional_branch().has_value()) c.set_optional_branch(RebuildBranch(*v.optional_branch()));

    {
        std::vector<int32_t> rs;
        const auto view = v.repeated_scalar();
        for (int64_t i = 0; i < view.size(); ++i) rs.push_back(view[i]);
        c.set_repeated_scalar(std::move(rs));
    }
    {
        std::vector<std::string> rs;
        const auto view = v.repeated_string();
        for (int64_t i = 0; i < view.size(); ++i) rs.emplace_back(view[i]);
        c.set_repeated_string(std::move(rs));
    }
    {
        std::vector<std::string> rb;
        const auto view = v.repeated_bytes();
        for (int64_t i = 0; i < view.size(); ++i) rb.emplace_back(view[i]);
        c.set_repeated_bytes(std::move(rb));
    }
    c.set_repeated_struct(RebuildLeaves(v.repeated_struct()));

    {
        std::vector<std::pair<std::string, int32_t>> ms;
        const auto view = v.map_scalar();
        for (int64_t i = 0; i < view.size(); ++i)
            ms.emplace_back(std::string(view.key(i)), view.value(i));
        c.set_map_scalar(std::move(ms));
    }
    {
        std::vector<std::pair<std::string, gen::Leaf>> ms;
        const auto view = v.map_struct();
        for (int64_t i = 0; i < view.size(); ++i)
            ms.emplace_back(std::string(view.key(i)), RebuildLeaf(view.value(i)));
        c.set_map_struct(std::move(ms));
    }

    c.set_flattened_struct_list(RebuildLeaves(v.flattened_struct_list()));
    c.set_optional_flattened_struct_list(RebuildLeaves(v.optional_flattened_struct_list()));

    {
        std::vector<std::vector<gen::Leaf>> nested;
        const auto outer = v.nested_struct_lists();
        for (int64_t i = 0; i < outer.size(); ++i) nested.push_back(RebuildLeaves(outer[i]));
        c.set_nested_struct_lists(std::move(nested));
    }
    {
        std::vector<std::vector<std::vector<gen::Leaf>>> d3;
        const auto outer = v.depth3_struct_lists();
        for (int64_t i = 0; i < outer.size(); ++i) {
            std::vector<std::vector<gen::Leaf>> mid;
            const auto mid_view = outer[i];
            for (int64_t j = 0; j < mid_view.size(); ++j) mid.push_back(RebuildLeaves(mid_view[j]));
            d3.push_back(std::move(mid));
        }
        c.set_depth3_struct_lists(std::move(d3));
    }

    c.set_message_flattened_point(RebuildPoint(v.message_flattened_point()));
    c.set_field_flattened_position(RebuildPosition(v.field_flattened_position()));
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// The GIR-6 forcing test. Exercises the migrated ToArrowRow() + `<Class>View`
// getters end to end through the real Codec, using the real CompositeCoverage
// fixture (not placeholder names).
// ---------------------------------------------------------------------------
TEST(ViewVisitor, RoundTripsViaCodec) {
    const gen::CompositeCoverage original = fx::MakeComposite();

    // message -> ToArrowRow -> Arrow (found via ADL, emitted by GIR-6 visitor).
    const fletcher::ArrowRow row = ToArrowRow(original);

    auto schema = coverage_test::ImportNano(gen::CompositeCoverageSchema());
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(static_cast<int>(row.size()), schema->num_fields());

    fletcher::Codec codec(schema);

    // ToArrowRow -> EncodeRow -> DecodeRow. `encoded` outlives the view (decode
    // may borrow zero-copy).
    const fletcher::EncodedRow encoded = codec.EncodeRow(row);
    ASSERT_FALSE(encoded.empty());
    fletcher::ArrowRow decoded = codec.DecodeRow(encoded);

    const gen::CompositeCoverageView view(std::move(decoded));

    // ---- reconstruct through the generated view getters, then Equals --------
    const gen::CompositeCoverage reconstructed = RebuildComposite(view);
    EXPECT_EQ(reconstructed.Encode(), original.Encode())
        << "reconstruct-through-view round trip is not a fixpoint";

    // ---- explicit read-backs: scalar, nullable struct inner ----------------
    EXPECT_EQ(view.scalars().int32_value(), fx::kInt32);
    EXPECT_EQ(view.scalars().string_value(), std::string_view(fx::StringValue()));
    EXPECT_EQ(view.scalars().status(), fx::kTopLevelStatusWarn);

    // nullable struct PRESENT (optional_scalars is set in the fixture): read an
    // inner scalar field back through the nested view.
    ASSERT_TRUE(view.optional_scalars().has_value());
    EXPECT_EQ(view.optional_scalars()->int32_value(), fx::kInt32);
    // nullable struct ABSENT (optional_branch is unset in the fixture).
    EXPECT_FALSE(view.optional_branch().has_value());

    // non-nullable nested struct chain inner field.
    EXPECT_EQ(view.branch().leaf().id(), 1);
    EXPECT_EQ(view.branch().leaf().label(), std::string_view("root"));

    // ---- map keys + values (scalar-value map) ------------------------------
    {
        std::map<std::string, int32_t> got;
        const auto ms = view.map_scalar();
        for (int64_t i = 0; i < ms.size(); ++i) got[std::string(ms.key(i))] = ms.value(i);
        const std::map<std::string, int32_t> expected{{"a", 1}, {"b", 2}};
        EXPECT_EQ(got, expected);
    }

    // ---- map keys + message values (message-value map) ---------------------
    {
        const auto ms = view.map_struct();
        ASSERT_EQ(ms.size(), 1);
        EXPECT_EQ(ms.key(0), std::string_view("k"));
        EXPECT_EQ(ms.value(0).id(), 7);
        EXPECT_EQ(ms.value(0).label(), std::string_view("mk"));
    }

    // ---- nested-list inner struct fields (depth 2 + depth 3) ---------------
    {
        const auto nsl = view.nested_struct_lists();
        ASSERT_EQ(nsl.size(), 2);
        ASSERT_EQ(nsl[0].size(), 1);
        EXPECT_EQ(nsl[0][0].id(), 9);
        EXPECT_EQ(nsl[1].size(), 0);
    }
    {
        const auto d3 = view.depth3_struct_lists();
        ASSERT_EQ(d3.size(), 1);
        ASSERT_EQ(d3[0].size(), 1);
        ASSERT_EQ(d3[0][0].size(), 1);
        EXPECT_EQ(d3[0][0][0].id(), 10);
    }
}
