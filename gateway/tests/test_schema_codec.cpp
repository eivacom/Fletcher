// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include "schema_codec.hpp"

#include <nanoarrow/nanoarrow.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

using fletcher::gateway::BuildArrowSchemaFromJson;
using fletcher::gateway::NanoarrowTypeToWireType;
using fletcher::gateway::WireTypeToNanoarrowType;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// BuildArrowSchemaFromJson — happy path
// ---------------------------------------------------------------------------

TEST(BuildArrowSchemaFromJsonTest, BuildsStructWithScalarFields) {
    json schema = {
        {"fields", json::array({
            {{"name", "id"},    {"wireType", 0x05}},  // INT64
            {{"name", "value"}, {"wireType", 0x0B}},  // DOUBLE
            {{"name", "label"}, {"wireType", 0x0C}},  // STRING
        })},
    };

    auto owned = BuildArrowSchemaFromJson(schema);

    ASSERT_NE(owned.get(), nullptr);
    EXPECT_EQ(owned->n_children, 3);
    EXPECT_STREQ(owned->children[0]->name, "id");
    EXPECT_STREQ(owned->children[1]->name, "value");
    EXPECT_STREQ(owned->children[2]->name, "label");
}

TEST(BuildArrowSchemaFromJsonTest, BuildsEmptyStruct) {
    json schema = {{"fields", json::array()}};
    auto owned = BuildArrowSchemaFromJson(schema);

    ASSERT_NE(owned.get(), nullptr);
    EXPECT_EQ(owned->n_children, 0);
}

// ---------------------------------------------------------------------------
// BuildArrowSchemaFromJson — error paths. These are the actual reason
// to unit-test this function: the integration test only sends
// well-formed protoc-gen schemas, so the error messages are otherwise
// untested.
// ---------------------------------------------------------------------------

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldsKeyMissing) {
    json schema = {{"name", "Telemetry"}};
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldsIsNotArray) {
    json schema = {{"fields", "not an array"}};
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldHasNoName) {
    json schema = {
        {"fields", json::array({
            {{"wireType", 0x05}},
        })},
    };
    try {
        BuildArrowSchemaFromJson(schema);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        // The error must name the index so the client can locate the bad field.
        const std::string what = e.what();
        EXPECT_NE(what.find("index 0"), std::string::npos) << what;
        EXPECT_NE(what.find("name"),    std::string::npos) << what;
    }
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldNameIsNotString) {
    json schema = {
        {"fields", json::array({
            {{"name", 42}, {"wireType", 0x05}},
        })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldHasNoWireType) {
    json schema = {
        {"fields", json::array({
            {{"name", "id"}, {"wireType", 0x05}},
            {{"name", "broken"}},
        })},
    };
    try {
        BuildArrowSchemaFromJson(schema);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("index 1"),  std::string::npos) << what;
        EXPECT_NE(what.find("wireType"), std::string::npos) << what;
    }
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldWireTypeIsNotInteger) {
    json schema = {
        {"fields", json::array({
            {{"name", "id"}, {"wireType", "INT64"}},
        })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsOnUnsupportedWireType) {
    // 0x20 = STRUCT. Nested types are not supported in publisher-
    // supplied schemas; the parser must reject them rather than
    // silently producing a broken nanoarrow setup.
    json schema = {
        {"fields", json::array({
            {{"name", "nested"}, {"wireType", 0x20}},
        })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsOnUnknownWireType) {
    json schema = {
        {"fields", json::array({
            {{"name", "mystery"}, {"wireType", 0xFE}},
        })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// WireType ↔ ArrowType round-trip. One test for every scalar wireType
// the gateway claims to support — guards against the two switch
// statements drifting apart.
// ---------------------------------------------------------------------------

TEST(WireTypeRoundTripTest, ScalarWireTypesRoundTripThroughNanoarrowType) {
    constexpr int kScalarWireTypes[] = {
        0x01,  // BOOL
        0x02,  // INT8
        0x03,  // INT16
        0x04,  // INT32
        0x05,  // INT64
        0x06,  // UINT8
        0x07,  // UINT16
        0x08,  // UINT32
        0x09,  // UINT64
        0x0A,  // FLOAT
        0x0B,  // DOUBLE
        0x0C,  // STRING
        0x0D,  // BINARY
        0x18,  // LARGE_STRING
        0x19,  // LARGE_BINARY
    };

    for (int wt : kScalarWireTypes) {
        enum ArrowType t = WireTypeToNanoarrowType(wt);
        int round_tripped = NanoarrowTypeToWireType(t);
        EXPECT_EQ(round_tripped, wt)
            << "wireType 0x" << std::hex << wt
            << " did not round-trip through NanoarrowType";
    }
}
