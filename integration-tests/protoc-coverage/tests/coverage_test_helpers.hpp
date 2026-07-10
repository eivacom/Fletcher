// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-2 shared test helpers. GIR-1 kept `ImportNano` / `ReadFileBytes` in the
// harness TU's anonymous namespace; the parity oracle (test_parity_oracle.cpp)
// needs the same two helpers and must not re-implement them subtly differently.
// They are promoted here as inline free functions so the oracle can import
// generated schemas and read committed golden bytes without a use-after-free.
//
// NOTE: `ToArrowRow(const gen::<Message>&)` is emitted by the generator into
// coverage.fletcher.arrow.pb.h and found via ADL — it is intentionally NOT
// redeclared here.
#pragma once

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

#include <fletcher/pubsub/owned_schema.hpp>

namespace coverage_test {

// Converts a nanoarrow OwnedSchema to a shared_ptr<arrow::Schema> via the Arrow
// C Data Interface. Returns nullptr (and records a gtest failure) on error.
inline std::shared_ptr<arrow::Schema> ImportNano(fletcher::OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

// Reads a whole file into a byte vector. Returns an empty vector (and records a
// gtest failure) when the file cannot be opened — callers ASSERT the result is
// non-empty, so a missing golden fails red for the right reason.
inline std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

}  // namespace coverage_test
