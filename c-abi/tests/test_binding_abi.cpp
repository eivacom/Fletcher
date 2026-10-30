// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-0's forcing test for the shim itself. The suite is deliberately thin:
// at kickoff the whole exported surface is the version function, and the point
// of the item is that the LANE runs, on both platforms, before there is
// anything real to run it on.
#include <gtest/gtest.h>

#include "fletcher/abi/binding.h"

namespace {

// The compiled-in constant and the shim's answer are two different things the
// moment a binding loads a shim it was not built against — which is the whole
// reason the function exists. Here they are built together, so they must agree.
TEST(BindingAbi, VersionMatchesHeader) {
    EXPECT_EQ(fl_binding_abi_version(), FL_BINDING_ABI_VERSION);
}

// The packing is (major << 16) | minor, and a binding on the other side of the
// ABI decodes it with exactly that arithmetic. Stated as a test so a later
// widening of the version word is a red row and not a silent reinterpretation.
TEST(BindingAbi, VersionIsPackedMajorMinor) {
    const uint32_t v = fl_binding_abi_version();
    EXPECT_EQ(v >> 16, static_cast<uint32_t>(FL_BINDING_ABI_VERSION_MAJOR));
    EXPECT_EQ(v & 0xFFFFu, static_cast<uint32_t>(FL_BINDING_ABI_VERSION_MINOR));
}

}  // namespace
