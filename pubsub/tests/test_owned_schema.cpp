// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/pubsub/owned_schema.hpp>
#include <limits>
#include <stdexcept>

using namespace fletcher;

// #54 — OwnedSchema::DeepCopy must surface a failed ArrowSchemaDeepCopy rather
// than silently returning an empty schema.
//
// Forcing input: a schema declaring an impossibly large child count. nanoarrow's
// ArrowSchemaDeepCopy copies format/name/metadata (all valid here), then calls
// ArrowSchemaAllocateChildren(schema_out, n_children), which requests
// n_children * sizeof(ArrowSchema*) bytes from malloc BEFORE dereferencing the
// (null) children pointer. That allocation cannot succeed, so nanoarrow returns
// ENOMEM. Before the fix that status is discarded and DeepCopy returns an empty
// OwnedSchema (no throw); after the fix DeepCopy throws std::runtime_error.
TEST(OwnedSchemaTest, DeepCopyFailureThrows) {
    ArrowSchema malformed{};
    malformed.format = "i";
    malformed.name = nullptr;
    malformed.metadata = nullptr;
    malformed.flags = 0;
    malformed.n_children =
        std::numeric_limits<int64_t>::max() / static_cast<int64_t>(sizeof(ArrowSchema*));
    malformed.children = nullptr;
    malformed.dictionary = nullptr;
    malformed.release = nullptr;
    malformed.private_data = nullptr;

    EXPECT_THROW(
        {
            auto copy = OwnedSchema::DeepCopy(&malformed);
            (void)copy;
        },
        std::runtime_error);
}
