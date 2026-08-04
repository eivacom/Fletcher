// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// --fletcher_opt=metadata_from_option (issue #117) — custom proto options
// copied through to Arrow schema/field metadata.
//
// The protoc unit tests cover the resolution semantics in-process. This TU is
// the only place the COMPILED GENERATED C++ is executed, so it is what proves
// the emitted <Class>Schema() really carries the mapped keys — including that
// the emitted string literals escape correctly. The mapping rules live in this
// harness's CMakeLists (_META_RULES).

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <fletcher/pubsub/owned_schema.hpp>
#include <memory>
#include <string>

#include "option_metadata.fletcher.pb.h"

namespace {

using fletcher::OwnedSchema;

std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

// Metadata lookup that reports absence distinctly from an empty value.
std::string Meta(const std::shared_ptr<const arrow::KeyValueMetadata>& md, const std::string& key) {
    if (md == nullptr || !md->Contains(key)) return "<missing>";
    return md->Get(key).ValueOr("<error>");
}

std::string FieldMeta(const std::shared_ptr<arrow::Schema>& schema, const std::string& field,
                      const std::string& key) {
    const auto f = schema->GetFieldByName(field);
    if (f == nullptr) return "<no-such-field>";
    return Meta(f->metadata(), key);
}

std::shared_ptr<arrow::Schema> SampleSchema() {
    return ImportNano(fletcher_gen::integration::metadata::SampleSchema());
}

}  // namespace

TEST(MetadataOptionsTest, SchemaLevelKeysComeFromTheMessagesOwnOptions) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(Meta(schema->metadata(), "x:group"), "g0");
    // The four builtin keys are untouched by the feature.
    EXPECT_EQ(Meta(schema->metadata(), "proto_package"), "integration.metadata");
    EXPECT_EQ(Meta(schema->metadata(), "proto_message"), "Sample");
}

TEST(MetadataOptionsTest, FlattenedWrapperFieldCarriesTheWrapperMessagesMetadata) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);

    // Stamp is (fletcher.flatten) over a single int64, so `t` is a bare int64
    // column — the struct wrapper the annotation was declared on is gone from
    // the Arrow schema, and the metadata still has to land here.
    const auto t = schema->GetFieldByName("t");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->type()->id(), arrow::Type::INT64);
    EXPECT_EQ(Meta(t->metadata(), "x:kind"), "KIND_ALPHA");
    EXPECT_EQ(Meta(t->metadata(), "x:unit"), "s");
    // Builtins still present alongside the mapped keys.
    EXPECT_EQ(Meta(t->metadata(), "field_number"), "1");
    EXPECT_EQ(Meta(t->metadata(), "field_id"), "1");
}

TEST(MetadataOptionsTest, EnumSubFieldIsTheVerbatimValueName) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    // Not "1", not a stripped or lowercased form.
    EXPECT_EQ(FieldMeta(schema, "t", "x:kind"), "KIND_ALPHA");
}

TEST(MetadataOptionsTest, ArrowExtensionNameResolvesThroughEnumValueOptions) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    // Pos declares enc = ENC_GEO, whose own EnumValueOptions carry the name.
    EXPECT_EQ(FieldMeta(schema, "pos", "ARROW:extension:name"), "geoarrow.point");
}

TEST(MetadataOptionsTest, ExtensionNameFallsBackWhenThePreferredHopIsEmpty) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    // Stamp declares enc = ENC_F64, which has no enc_opts, so the earlier
    // kind-derived rule stands. Fallback is expressed purely by rule order.
    EXPECT_EQ(FieldMeta(schema, "t", "ARROW:extension:name"), "fx.alpha");
}

TEST(MetadataOptionsTest, JsonValueSurvivesCppStringLiteralEscaping) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    // The option value contains double quotes. If the emitter did not escape
    // them the generated header would not compile; if it over-escaped, the
    // value would come back mangled. Exact match is the assertion.
    EXPECT_EQ(FieldMeta(schema, "pos", "ARROW:extension:metadata"), R"({"crs":"EPSG:4326"})");
}

TEST(MetadataOptionsTest, FieldScopeOverridesInheritedFieldTypeScopePerKey) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    EXPECT_EQ(FieldMeta(schema, "heading", "x:unit"), "deg");
    // A bare double has no message type, so nothing is inherited for it.
    EXPECT_EQ(FieldMeta(schema, "heading", "x:kind"), "<missing>");
}

TEST(MetadataOptionsTest, RepeatedFlattenPutsMetadataOnTheListFieldOnly) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);

    const auto stamps = schema->GetFieldByName("stamps");
    ASSERT_NE(stamps, nullptr);
    ASSERT_EQ(stamps->type()->id(), arrow::Type::LIST);
    EXPECT_EQ(Meta(stamps->metadata(), "x:unit"), "s");

    // The list element carries no metadata: it has no FieldInfo and therefore no
    // attachment point. Documented limitation, pinned here so a future change to
    // it is a deliberate decision rather than an accident.
    const auto item = stamps->type()->field(0);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(Meta(item->metadata(), "x:unit"), "<missing>");
}

TEST(MetadataOptionsTest, MapColumnCarriesOnlyTheBuiltinKeys) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);
    // A map field's message type is the synthetic MapEntry, which is never a
    // metadata carrier.
    EXPECT_EQ(FieldMeta(schema, "extras", "field_number"), "5");
    EXPECT_EQ(FieldMeta(schema, "extras", "x:unit"), "<missing>");
    EXPECT_EQ(FieldMeta(schema, "extras", "x:kind"), "<missing>");
}

TEST(MetadataOptionsTest, FlattenFieldInlinedLeavesInheritTheOuterWrapper) {
    auto schema = ImportNano(fletcher_gen::integration::metadata::PosSchema());
    ASSERT_NE(schema, nullptr);

    // Pos.coord is (fletcher.flatten_field), so x/y are inlined and the outer
    // `coord` field does not exist in Arrow. Coord's message annotation reaches
    // both leaves through the flatten chain.
    EXPECT_EQ(FieldMeta(schema, "x", "field_id"), "1.1");
    EXPECT_EQ(FieldMeta(schema, "y", "field_id"), "1.2");
    EXPECT_EQ(FieldMeta(schema, "x", "x:unit"), "m");
    EXPECT_EQ(FieldMeta(schema, "y", "x:unit"), "m");
}

TEST(MetadataOptionsTest, NestedStructChildMetadataSurvivesTheDeepCopy) {
    auto schema = SampleSchema();
    ASSERT_NE(schema, nullptr);

    // `pos` is a non-flattened struct: the generated C++ deep-copies PosSchema()
    // and then overwrites the CHILD's metadata with the field pairs. The
    // grandchildren (x/y) must keep theirs.
    const auto pos = schema->GetFieldByName("pos");
    ASSERT_NE(pos, nullptr);
    ASSERT_EQ(pos->type()->id(), arrow::Type::STRUCT);
    EXPECT_EQ(Meta(pos->metadata(), "x:kind"), "KIND_ALPHA");

    const auto& pos_struct = static_cast<const arrow::StructType&>(*pos->type());
    const auto x = pos_struct.GetFieldByName("x");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(Meta(x->metadata(), "x:unit"), "m");
}
