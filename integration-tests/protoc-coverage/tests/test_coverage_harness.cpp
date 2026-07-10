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

// Returns the value mapped to `key` in a decoded scalar map, or nullopt.
template <typename Pairs>
std::optional<int32_t> MapLookup(const Pairs& pairs, std::string_view key) {
    for (const auto& kv : pairs)
        if (kv.first == key) return kv.second;
    return std::nullopt;
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
