// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-0's forcing test for the shim itself. The suite is deliberately thin:
// at kickoff the whole exported surface is the version function, and the point
// of the item is that the LANE runs, on both platforms, before there is
// anything real to run it on.
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

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

// D9S-1 (d9 L16, D23). The build asks itself: `fl_binding_variant` is callable
// before anything else, as `fl_binding_abi_version` is, and answers the build it
// is. This file is compiled into BOTH test executables - `c_abi_tests` (the
// shipped shim) and `c_abi_probe_tests` (the test-only probe shim, ctest names
// prefixed `probe.`) - and both are the edge build, so both answer edge.
//
// FIRST CALL. This row makes the query the FIRST call into the shim in its
// process: no handle, no provider, no other entry point comes before it. The
// claim holds when the row runs alone, which is how ctest runs it
// (`gtest_discover_tests` gives every row its own process). The expected values
// are written as the literals the ABI promises, not only as the enumerators, so
// a renumbering is a red row: a binding built against 1 = edge must keep
// reading edge, and 0 is never a build, so a zeroed result can never pass as one.
TEST(BindingAbi, TheVariantIsAnsweredBeforeAnyOtherCall) {
    const fl_variant variant = fl_binding_variant();
    EXPECT_NE(static_cast<int>(variant), 0) << "0 is not a build";
    EXPECT_EQ(variant, FL_VARIANT_EDGE);
    EXPECT_EQ(static_cast<int>(variant), 1) << "edge is 1 on the wire, by the header";
    EXPECT_EQ(static_cast<int>(FL_VARIANT_SERVER), 2);
    // A constant: a second ask answers the same, so the answer is not a one-shot.
    EXPECT_EQ(fl_binding_variant(), variant);
}

// MANY THREADS. Eight threads are released together (they spin on one flag, so
// they overlap instead of running one after another) and each asks the variant
// and the version several times. Every answer must be edge and the header's
// version: the query touches no state, so there is nothing for a race to find,
// and this is the row that says "callable from any thread" is true. Results are
// collected and asserted on this thread, not inside the workers.
TEST(BindingAbi, TheVariantAndVersionAnswerAlikeFromManyThreads) {
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kAsksPerThread = 1000;

    struct Answers {
        std::size_t wrong_variant = 0;
        std::size_t wrong_version = 0;
    };
    std::vector<Answers> answers(kThreads);
    std::atomic<bool> go{false};
    std::atomic<std::size_t> ready{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load()) std::this_thread::yield();
            for (std::size_t i = 0; i < kAsksPerThread; ++i) {
                if (fl_binding_variant() != FL_VARIANT_EDGE) ++answers[t].wrong_variant;
                if (fl_binding_abi_version() != FL_BINDING_ABI_VERSION) ++answers[t].wrong_version;
            }
        });
    }
    while (ready.load() < kThreads) std::this_thread::yield();
    go.store(true);
    for (std::thread& thread : threads) thread.join();

    for (std::size_t t = 0; t < kThreads; ++t) {
        EXPECT_EQ(answers[t].wrong_variant, 0u) << "thread " << t << " was told a build other than edge";
        EXPECT_EQ(answers[t].wrong_version, 0u) << "thread " << t << " was told a version other than the header's";
    }
}

}  // namespace
