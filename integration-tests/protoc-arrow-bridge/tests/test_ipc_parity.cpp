// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// --fletcher_opt=ipc — emitted .ipc schema files.
//
// The plugin documents the .ipc files as byte-identical to the schema bytes
// providers announce at runtime (SerializeSchemaIpc over the generated
// <Class>Schema()). The plugin builds its schema in-process from the same
// proto descriptors the generated code is derived from; this test is the
// proof that the two paths actually agree on every byte.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <fstream>
#include <vector>

#include "nested.fletcher.pb.h"
#include "option_metadata.fletcher.pb.h"
#include "pubsub.fletcher.pb.h"

namespace {

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

}  // namespace

TEST(IpcParityTest, FlatMessageIpcFileMatchesRuntimeSchemaBytes) {
    const std::filesystem::path path = GeneratedDir() / "pubsub.Telemetry.ipc";
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    const std::vector<uint8_t> file_bytes = ReadFileBytes(path);
    ASSERT_FALSE(file_bytes.empty());

    fletcher::OwnedSchema schema = fletcher_gen::integration::pubsub::TelemetrySchema();
    const std::vector<uint8_t> runtime_bytes = fletcher::SerializeSchemaIpc(schema.get());

    EXPECT_EQ(file_bytes, runtime_bytes);
}

TEST(IpcParityTest, NestedMessageIpcFileMatchesRuntimeSchemaBytes) {
    const std::filesystem::path path = GeneratedDir() / "nested.Location.ipc";
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    const std::vector<uint8_t> file_bytes = ReadFileBytes(path);
    ASSERT_FALSE(file_bytes.empty());

    fletcher::OwnedSchema schema = fletcher_gen::integration::LocationSchema();
    const std::vector<uint8_t> runtime_bytes = fletcher::SerializeSchemaIpc(schema.get());

    EXPECT_EQ(file_bytes, runtime_bytes);
}

TEST(IpcParityTest, MappedMetadataIpcFileMatchesRuntimeSchemaBytes) {
    // #117: the two emission paths build their metadata from one shared pair
    // vector, but only a byte comparison proves they agree once mapped keys are
    // present — it is the single check that catches both a key-ORDER divergence
    // and a C++ string-literal ESCAPING bug (option_metadata.proto's ext_meta
    // carries JSON with embedded quotes).
    const std::filesystem::path path = GeneratedDir() / "option_metadata.Sample.ipc";
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    const std::vector<uint8_t> file_bytes = ReadFileBytes(path);
    ASSERT_FALSE(file_bytes.empty());

    fletcher::OwnedSchema schema = fletcher_gen::integration::metadata::SampleSchema();
    const std::vector<uint8_t> runtime_bytes = fletcher::SerializeSchemaIpc(schema.get());

    EXPECT_EQ(file_bytes, runtime_bytes);
}

TEST(IpcParityTest, MappedMetadataOnFlattenedFieldsMatchesRuntimeSchemaBytes) {
    // Pos exercises (fletcher.flatten_field) inlining plus the JSON-valued
    // option, so this pins escaping and flatten-chain inheritance on both paths.
    const std::filesystem::path path = GeneratedDir() / "option_metadata.Pos.ipc";
    ASSERT_TRUE(std::filesystem::exists(path)) << path;

    const std::vector<uint8_t> file_bytes = ReadFileBytes(path);
    ASSERT_FALSE(file_bytes.empty());

    fletcher::OwnedSchema schema = fletcher_gen::integration::metadata::PosSchema();
    const std::vector<uint8_t> runtime_bytes = fletcher::SerializeSchemaIpc(schema.get());

    EXPECT_EQ(file_bytes, runtime_bytes);
}

TEST(IpcParityTest, EveryGeneratedHeaderStemHasIpcFiles) {
    // One .ipc per generated message: spot-check a representative set across
    // the fixture stems (maps, temporal, flatten) so a regression in the
    // emission loop — not just in one schema — is caught.
    const char* kExpected[] = {
        "pubsub.Telemetry.ipc",
        "nested.GeoPoint.ipc",
        "nested.Address.ipc",
        "nested.Location.ipc",
    };
    for (const char* name : kExpected) {
        EXPECT_TRUE(std::filesystem::exists(GeneratedDir() / name)) << name;
    }
}
