// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-1 forcing test: compile AND execute the plugin's generated C++ output for
// one broad coverage fixture. Encode a populated CompositeCoverage row,
// reconstruct it through the generated encoded-row constructor, convert it via
// ToArrowRow / the generated Arrow view, and confirm the emitted IPC schema is
// readable. This is a refactor guard, NOT a wire-byte oracle (byte identity and
// Encode()==EncodeRow() belong to GIR-2).

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>

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

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

std::filesystem::path GeneratedDir() { return std::filesystem::path(GENERATED_DIR_PATH); }

// Committed wire-contract goldens, shared with the parity oracle. GIR-4 decodes
// them through the generated EDGE constructor (the parity oracle decodes via the
// Codec/View surface), so the same bytes now guard both decode paths.
std::filesystem::path GoldenDir() { return std::filesystem::path(PARITY_GOLDEN_DIR_PATH); }

// Returns the value mapped to `key` in a decoded scalar map, or nullopt.
template <typename Pairs>
std::optional<int32_t> MapLookup(const Pairs& pairs, std::string_view key) {
    for (const auto& kv : pairs)
        if (kv.first == key) return kv.second;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Field-by-field equality between a source generated edge message (expected)
// and a message reconstructed through the generated edge constructor (actual).
// This is the GIR-4 decode oracle: it exercises the IR-driven EmitFieldDecode
// for every scalar / struct / list / map / nested-list shape and asserts the
// nullable presence axis, not just container sizes. Callee-before-caller so
// ordinary lookup resolves each nested overload.
// ---------------------------------------------------------------------------

void ExpectEquals(const gen::Leaf& e, const gen::Leaf& a) {
    EXPECT_EQ(a.id(), e.id());
    EXPECT_EQ(a.label(), e.label());
    EXPECT_EQ(a.status(), e.status());
}

void ExpectEquals(const gen::NestedEnums& e, const gen::NestedEnums& a) {
    EXPECT_EQ(a.state(), e.state());
}

void ExpectEquals(const gen::FlattenedPoint& e, const gen::FlattenedPoint& a) {
    EXPECT_DOUBLE_EQ(a.x(), e.x());
    EXPECT_DOUBLE_EQ(a.y(), e.y());
}

void ExpectEquals(const gen::FieldFlattenedPosition& e, const gen::FieldFlattenedPosition& a) {
    EXPECT_DOUBLE_EQ(a.x(), e.x());
    EXPECT_DOUBLE_EQ(a.y(), e.y());
}

void ExpectEquals(const gen::ServiceReply& e, const gen::ServiceReply& a) {
    EXPECT_EQ(a.accepted(), e.accepted());
    EXPECT_EQ(a.message(), e.message());
}

void ExpectEquals(const gen::ScalarCoverage& e, const gen::ScalarCoverage& a) {
    EXPECT_EQ(a.bool_value(), e.bool_value());
    EXPECT_EQ(a.int32_value(), e.int32_value());
    EXPECT_EQ(a.int64_value(), e.int64_value());
    EXPECT_EQ(a.uint32_value(), e.uint32_value());
    EXPECT_EQ(a.uint64_value(), e.uint64_value());
    EXPECT_EQ(a.sint32_value(), e.sint32_value());
    EXPECT_EQ(a.sint64_value(), e.sint64_value());
    EXPECT_EQ(a.fixed32_value(), e.fixed32_value());
    EXPECT_EQ(a.fixed64_value(), e.fixed64_value());
    EXPECT_EQ(a.sfixed32_value(), e.sfixed32_value());
    EXPECT_EQ(a.sfixed64_value(), e.sfixed64_value());
    EXPECT_FLOAT_EQ(a.float_value(), e.float_value());
    EXPECT_DOUBLE_EQ(a.double_value(), e.double_value());
    EXPECT_EQ(a.string_value(), e.string_value());
    EXPECT_EQ(a.bytes_value(), e.bytes_value());
    // optional primitives + WKT wrappers: presence AND value on both axes.
    EXPECT_EQ(a.optional_bool(), e.optional_bool());
    EXPECT_EQ(a.optional_int32(), e.optional_int32());
    EXPECT_EQ(a.optional_string(), e.optional_string());
    EXPECT_EQ(a.optional_bytes(), e.optional_bytes());
    EXPECT_EQ(a.wrapped_bool(), e.wrapped_bool());
    EXPECT_EQ(a.wrapped_int32(), e.wrapped_int32());
    EXPECT_EQ(a.wrapped_int64(), e.wrapped_int64());
    EXPECT_EQ(a.wrapped_uint32(), e.wrapped_uint32());
    EXPECT_EQ(a.wrapped_uint64(), e.wrapped_uint64());
    EXPECT_EQ(a.wrapped_float(), e.wrapped_float());
    EXPECT_EQ(a.wrapped_double(), e.wrapped_double());
    EXPECT_EQ(a.wrapped_string(), e.wrapped_string());
    EXPECT_EQ(a.wrapped_bytes(), e.wrapped_bytes());
    EXPECT_EQ(a.timestamp_value(), e.timestamp_value());
    EXPECT_EQ(a.duration_value(), e.duration_value());
    EXPECT_EQ(a.status(), e.status());
    EXPECT_EQ(a.nested_status(), e.nested_status());
}

void ExpectEquals(const gen::Branch& e, const gen::Branch& a) {
    ExpectEquals(e.leaf(), a.leaf());
    if (e.optional_leaf() != nullptr) {
        ASSERT_NE(a.optional_leaf(), nullptr);
        ExpectEquals(*e.optional_leaf(), *a.optional_leaf());
    } else {
        EXPECT_EQ(a.optional_leaf(), nullptr);
    }
    ASSERT_EQ(a.leaves().size(), e.leaves().size());
    for (size_t i = 0; i < e.leaves().size(); ++i) ExpectEquals(e.leaves()[i], a.leaves()[i]);
}

void ExpectEquals(const gen::CompositeCoverage& e, const gen::CompositeCoverage& a) {
    ExpectEquals(e.scalars(), a.scalars());

    if (e.optional_scalars() != nullptr) {
        ASSERT_NE(a.optional_scalars(), nullptr);
        ExpectEquals(*e.optional_scalars(), *a.optional_scalars());
    } else {
        EXPECT_EQ(a.optional_scalars(), nullptr);
    }

    ExpectEquals(e.branch(), a.branch());

    if (e.optional_branch() != nullptr) {
        ASSERT_NE(a.optional_branch(), nullptr);
        ExpectEquals(*e.optional_branch(), *a.optional_branch());
    } else {
        EXPECT_EQ(a.optional_branch(), nullptr);
    }

    // repeated scalar / string / bytes: order + values (vector operator==).
    EXPECT_EQ(a.repeated_scalar(), e.repeated_scalar());
    EXPECT_EQ(a.repeated_string(), e.repeated_string());
    EXPECT_EQ(a.repeated_bytes(), e.repeated_bytes());

    ASSERT_EQ(a.repeated_struct().size(), e.repeated_struct().size());
    for (size_t i = 0; i < e.repeated_struct().size(); ++i)
        ExpectEquals(e.repeated_struct()[i], a.repeated_struct()[i]);

    // map<string,int32>: vector-of-pairs order + values preserved exactly.
    EXPECT_EQ(a.map_scalar(), e.map_scalar());
    // map<string,Leaf>: key order + per-key Leaf field equality.
    ASSERT_EQ(a.map_struct().size(), e.map_struct().size());
    for (size_t i = 0; i < e.map_struct().size(); ++i) {
        EXPECT_EQ(a.map_struct()[i].first, e.map_struct()[i].first);
        ExpectEquals(e.map_struct()[i].second, a.map_struct()[i].second);
    }

    ASSERT_EQ(a.flattened_struct_list().size(), e.flattened_struct_list().size());
    for (size_t i = 0; i < e.flattened_struct_list().size(); ++i)
        ExpectEquals(e.flattened_struct_list()[i], a.flattened_struct_list()[i]);
    ASSERT_EQ(a.optional_flattened_struct_list().size(), e.optional_flattened_struct_list().size());
    for (size_t i = 0; i < e.optional_flattened_struct_list().size(); ++i)
        ExpectEquals(e.optional_flattened_struct_list()[i], a.optional_flattened_struct_list()[i]);

    // struct-leaf nested lists: depth 2.
    ASSERT_EQ(a.nested_struct_lists().size(), e.nested_struct_lists().size());
    for (size_t i = 0; i < e.nested_struct_lists().size(); ++i) {
        ASSERT_EQ(a.nested_struct_lists()[i].size(), e.nested_struct_lists()[i].size());
        for (size_t j = 0; j < e.nested_struct_lists()[i].size(); ++j)
            ExpectEquals(e.nested_struct_lists()[i][j], a.nested_struct_lists()[i][j]);
    }
    // struct-leaf nested lists: depth 3.
    ASSERT_EQ(a.depth3_struct_lists().size(), e.depth3_struct_lists().size());
    for (size_t i = 0; i < e.depth3_struct_lists().size(); ++i) {
        ASSERT_EQ(a.depth3_struct_lists()[i].size(), e.depth3_struct_lists()[i].size());
        for (size_t j = 0; j < e.depth3_struct_lists()[i].size(); ++j) {
            ASSERT_EQ(a.depth3_struct_lists()[i][j].size(), e.depth3_struct_lists()[i][j].size());
            for (size_t k = 0; k < e.depth3_struct_lists()[i][j].size(); ++k)
                ExpectEquals(e.depth3_struct_lists()[i][j][k], a.depth3_struct_lists()[i][j][k]);
        }
    }

    ExpectEquals(e.message_flattened_point(), a.message_flattened_point());
    ExpectEquals(e.field_flattened_position(), a.field_flattened_position());
}

void ExpectEquals(const gen::ServiceRequest& e, const gen::ServiceRequest& a) {
    ExpectEquals(e.payload(), a.payload());
}

// For one fixture: reconstruct through the generated edge constructor from both
// freshly-encoded bytes and the committed golden, assert field equality against
// the source, and assert the decode->encode round-trip is byte-identical (which
// also pins the golden to the current wire contract, tying decode to encode).
template <typename Row>
void ReconstructAndCheck(const Row& source, const char* golden_name) {
    SCOPED_TRACE(golden_name);

    const EncodedRow encoded = source.Encode();
    ASSERT_FALSE(encoded.empty());
    const Row from_encoded(encoded);
    ExpectEquals(source, from_encoded);
    EXPECT_EQ(from_encoded.Encode(), encoded) << "edge decode->encode is not a fixpoint";

    const std::vector<uint8_t> golden = ReadFileBytes(GoldenDir() / golden_name);
    ASSERT_FALSE(golden.empty());
    const Row from_golden(golden.data(), golden.size());
    ExpectEquals(source, from_golden);
    EXPECT_EQ(from_golden.Encode(), golden) << "golden decode->encode != golden: " << golden_name;
}

}  // namespace

TEST(CoverageHarness, GeneratedCppCompilesEncodesAndReconstructs) {
    const gen::CompositeCoverage row = fx::MakeComposite();

    // ---- Encode + reconstruct through the generated encoded-row path --------
    const EncodedRow encoded = row.Encode();
    ASSERT_FALSE(encoded.empty());

    const gen::CompositeCoverage decoded(encoded);

    // scalar primitives
    EXPECT_EQ(decoded.scalars().int32_value(), fx::kInt32);
    EXPECT_EQ(decoded.scalars().int64_value(), fx::kInt64);
    EXPECT_EQ(decoded.scalars().bool_value(), true);
    // string / bytes
    EXPECT_EQ(decoded.scalars().string_value(), fx::StringValue());
    EXPECT_EQ(decoded.scalars().bytes_value(), std::string_view(fx::BytesValue()));
    // optional set + unset
    ASSERT_TRUE(decoded.scalars().optional_int32().has_value());
    EXPECT_EQ(*decoded.scalars().optional_int32(), fx::kOptInt32);
    EXPECT_FALSE(decoded.scalars().optional_bool().has_value());
    // WKT wrapper (set + unset) + timestamp + duration
    ASSERT_TRUE(decoded.scalars().wrapped_int32().has_value());
    EXPECT_EQ(*decoded.scalars().wrapped_int32(), fx::kWrappedInt32);
    EXPECT_FALSE(decoded.scalars().wrapped_double().has_value());
    EXPECT_EQ(decoded.scalars().timestamp_value(), fx::kTimestampNs);
    EXPECT_EQ(decoded.scalars().duration_value(), fx::kDurationNs);
    // enum-as-int32 (package-scope + nested)
    EXPECT_EQ(decoded.scalars().status(), fx::kTopLevelStatusWarn);
    EXPECT_EQ(decoded.scalars().nested_status(), fx::kInnerStatusActive);

    // nested struct + optional struct (set) + repeated struct list-in-struct
    EXPECT_EQ(decoded.branch().leaf().id(), 1);
    EXPECT_EQ(decoded.branch().leaf().label(), "root");
    ASSERT_NE(decoded.branch().optional_leaf(), nullptr);
    EXPECT_EQ(decoded.branch().optional_leaf()->id(), 2);
    ASSERT_EQ(decoded.branch().leaves().size(), 2u);
    EXPECT_EQ(decoded.branch().leaves()[1].label(), "b");

    // optional message set + unset
    ASSERT_NE(decoded.optional_scalars(), nullptr);
    EXPECT_EQ(decoded.optional_scalars()->int32_value(), fx::kInt32);
    EXPECT_EQ(decoded.optional_branch(), nullptr);

    // repeated scalar / struct + empty container
    ASSERT_EQ(decoded.repeated_scalar().size(), 3u);
    EXPECT_EQ(decoded.repeated_scalar()[2], 30);
    EXPECT_EQ(decoded.repeated_string().size(), 2u);
    EXPECT_TRUE(decoded.repeated_bytes().empty());
    ASSERT_EQ(decoded.repeated_struct().size(), 2u);
    EXPECT_EQ(decoded.repeated_struct()[0].id(), 5);

    // map scalar / struct
    ASSERT_EQ(decoded.map_scalar().size(), 2u);
    EXPECT_EQ(MapLookup(decoded.map_scalar(), "a"), std::optional<int32_t>(1));
    EXPECT_EQ(MapLookup(decoded.map_scalar(), "b"), std::optional<int32_t>(2));
    ASSERT_EQ(decoded.map_struct().size(), 1u);
    EXPECT_EQ(decoded.map_struct()[0].first, "k");
    EXPECT_EQ(decoded.map_struct()[0].second.id(), 7);

    // message-level flatten (StructListWrapper -> list<struct>)
    ASSERT_EQ(decoded.flattened_struct_list().size(), 1u);
    EXPECT_EQ(decoded.flattened_struct_list()[0].id(), 8);
    ASSERT_EQ(decoded.optional_flattened_struct_list().size(), 1u);
    EXPECT_EQ(decoded.optional_flattened_struct_list()[0].id(), 11);

    // struct-leaf nested lists (depth 2 + depth 3)
    ASSERT_EQ(decoded.nested_struct_lists().size(), 2u);
    ASSERT_EQ(decoded.nested_struct_lists()[0].size(), 1u);
    EXPECT_EQ(decoded.nested_struct_lists()[0][0].id(), 9);
    EXPECT_TRUE(decoded.nested_struct_lists()[1].empty());
    ASSERT_EQ(decoded.depth3_struct_lists().size(), 1u);
    ASSERT_EQ(decoded.depth3_struct_lists()[0][0].size(), 1u);
    EXPECT_EQ(decoded.depth3_struct_lists()[0][0][0].id(), 10);

    // message-level flatten (multi-field -> struct) + field-level flatten
    EXPECT_DOUBLE_EQ(decoded.message_flattened_point().x(), 1.25);
    EXPECT_DOUBLE_EQ(decoded.message_flattened_point().y(), 2.5);
    EXPECT_DOUBLE_EQ(decoded.field_flattened_position().x(), 3.75);
    EXPECT_DOUBLE_EQ(decoded.field_flattened_position().y(), 4.5);

    // ---- Reconstruct EVERY active fixture + committed golden through the -----
    // ---- generated edge constructor (the GIR-4 decode forcing oracle) -------
    // Mirrors the parity oracle's fixture set so every decode SHAPE and every
    // null/empty axis is reconstructed and round-trips byte-identically.
    ReconstructAndCheck(fx::MakeScalars(), "coverage.ScalarCoverage.v1.bin");
    ReconstructAndCheck(fx::MakeScalarsAllSet(), "coverage.ScalarCoverage.all-set.v1.bin");
    ReconstructAndCheck(fx::MakeComposite(), "coverage.CompositeCoverage.v1.bin");
    ReconstructAndCheck(fx::MakeCompositeWithAlternateNullsAndEmpties(),
                        "coverage.CompositeCoverage.alternate-null-empty.v1.bin");
    ReconstructAndCheck(fx::MakeCompositeWithMapsNonSorted(),
                        "coverage.CompositeCoverage.maps-non-sorted.v1.bin");
    ReconstructAndCheck(fx::MakeBranch(), "coverage.Branch.v1.bin");
    ReconstructAndCheck(fx::MakeLeaf(42, "leaf", fx::kTopLevelStatusError), "coverage.Leaf.v1.bin");
    ReconstructAndCheck(fx::MakeNestedEnums(), "coverage.NestedEnums.v1.bin");
    ReconstructAndCheck(fx::MakeFlattenedPoint(), "coverage.FlattenedPoint.v1.bin");
    ReconstructAndCheck(fx::MakeFieldFlattenedPosition(), "coverage.FieldFlattenedPosition.v1.bin");
    ReconstructAndCheck(fx::MakeServiceRequest(), "coverage.ServiceRequest.v1.bin");
    ReconstructAndCheck(fx::MakeServiceReply(), "coverage.ServiceReply.v1.bin");

    // ---- Arrow view + ToArrowRow (Arrow-side representation is usable) -------
    const ArrowRow arrow_row = ToArrowRow(row);
    const auto schema = ImportNano(gen::CompositeCoverageSchema());
    ASSERT_NE(schema, nullptr);
    ASSERT_EQ(static_cast<int>(arrow_row.size()), schema->num_fields());
    ASSERT_NE(arrow_row[0], nullptr);
    EXPECT_TRUE(arrow_row[0]->is_valid);  // `scalars` struct present

    gen::CompositeCoverageView view(RoundTrip(row.Encode(), gen::CompositeCoverageSchema()));
    EXPECT_EQ(view.scalars().int32_value(), fx::kInt32);
    EXPECT_EQ(view.scalars().status(), fx::kTopLevelStatusWarn);
    EXPECT_EQ(view.branch().leaf().id(), 1);
    EXPECT_EQ(view.repeated_scalar().size(), 3);
    EXPECT_EQ(view.repeated_scalar()[2], 30);

    // ---- IPC schema files present + readable as an Arrow schema -------------
    for (const char* name : {"coverage.ScalarCoverage.ipc", "coverage.CompositeCoverage.ipc"}) {
        const auto path = GeneratedDir() / name;
        ASSERT_TRUE(std::filesystem::exists(path)) << path;
        const auto bytes = ReadFileBytes(path);
        ASSERT_FALSE(bytes.empty()) << name;

        OwnedSchema nano = DeserializeSchemaIpc(bytes.data(), bytes.size());
        const auto ipc_schema = ImportNano(std::move(nano));
        ASSERT_NE(ipc_schema, nullptr) << name;
        EXPECT_GT(ipc_schema->num_fields(), 0) << name;
    }

    // The CompositeCoverage IPC schema exposes the expected top-level fields.
    OwnedSchema composite_nano =
        [] {
            const auto bytes = ReadFileBytes(GeneratedDir() / "coverage.CompositeCoverage.ipc");
            return DeserializeSchemaIpc(bytes.data(), bytes.size());
        }();
    const auto composite_schema = ImportNano(std::move(composite_nano));
    ASSERT_NE(composite_schema, nullptr);
    EXPECT_NE(composite_schema->GetFieldByName("scalars"), nullptr);
    EXPECT_NE(composite_schema->GetFieldByName("branch"), nullptr);
    EXPECT_NE(composite_schema->GetFieldByName("map_struct"), nullptr);
}
