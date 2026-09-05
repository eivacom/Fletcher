// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/core/status.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <limits>
#include <stdexcept>
#include <string>

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
// OwnedSchema (no throw); after the fix DeepCopy throws.
//
// WHAT it throws is the second thing pinned here, and it is not decoration.
// DeepCopy is public, and per spec §3.3 it is the ONLY memory-safe way for a
// language binding to consume a borrowed SharedSchema, so it is a seam entry
// point in everything but name. Spec §5.1 says the seam's failures are one
// typed error carrying one numbered cause, so an untyped std::runtime_error out
// of this function is a hole in the taxonomy at exactly the call a binding is
// told to make. kInternal is the number, and it is the number this failure
// already reached seam callers as: every in-tree call site sits inside a
// TranslateSeamFailure, whose std::exception arm maps to kInternal. So the type
// becomes truthful and no status any caller can already observe changes.
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

    // The typed form. PubSubError derives from std::runtime_error, so the
    // assertion above holds either way -- which is why the number is asserted
    // separately: it is the part a boundary carries.
    try {
        auto copy = OwnedSchema::DeepCopy(&malformed);
        (void)copy;
        FAIL() << "DeepCopy accepted a schema whose children cannot be allocated";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInternal);
        EXPECT_NE(std::string(e.what()).find("OwnedSchema::DeepCopy"), std::string::npos);
    } catch (const std::exception& e) {
        FAIL() << "DeepCopy threw an untyped exception a boundary has no number for: " << e.what();
    }
}
