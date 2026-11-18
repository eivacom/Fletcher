// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6d-2: the C# model the plugin emits for coverage.proto (`csharp,csharp_model_only`)
// must be byte-identical to the COMMITTED golden (CS_GOLDEN_PATH).
//
// That golden is not checked for its own sake. dotnet/tests/Fletcher.Tests compiles it
// and encodes the coverage fixture rows through it and the native codec, comparing the
// bytes with the .v1.bin goldens beside it, which the C++ generated class wrote
// (test_parity_oracle.cpp). This test is what makes the C# compiled there the plugin's
// real output today, so that comparison is C# against C++ and not against a stale copy.
//
// Regenerate with FLETCHER_REGEN_GOLDEN=1 (overwrites the golden and skips), then
// review the diff as source. Never runs in normal ctest.

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// First differing line (1-based) and the two line texts, for a readable failure.
std::string FirstDiff(const std::string& a, const std::string& b) {
    std::istringstream sa(a), sb(b);
    std::string la, lb;
    int line = 0;
    while (true) {
        const bool ga = static_cast<bool>(std::getline(sa, la));
        const bool gb = static_cast<bool>(std::getline(sb, lb));
        ++line;
        if (!ga && !gb) return "(no line-level difference; check trailing bytes)";
        if (ga != gb || la != lb) {
            std::ostringstream o;
            o << "first difference at line " << line << ":\n"
              << "  generated: " << (ga ? la : "<EOF>") << "\n"
              << "  golden:    " << (gb ? lb : "<EOF>");
            return o.str();
        }
    }
}

}  // namespace

TEST(CsGolden, ModelByteIdentical) {
    const std::string generated_path = GENERATED_CS_PATH;
    const std::string golden_path = CS_GOLDEN_PATH;

    const std::string generated = ReadFile(generated_path);
    ASSERT_FALSE(generated.empty()) << "generated C# not found or empty: " << generated_path
                                    << " (was the plugin run with --fletcher_opt=csharp?)";

    if (const char* regen = std::getenv("FLETCHER_REGEN_GOLDEN"); regen && *regen) {
        std::ofstream out(golden_path, std::ios::binary);
        out << generated;
        GTEST_SKIP() << "FLETCHER_REGEN_GOLDEN set: rewrote golden " << golden_path;
    }

    const std::string golden = ReadFile(golden_path);
    ASSERT_FALSE(golden.empty()) << "committed golden not found or empty: " << golden_path;
    EXPECT_EQ(generated, golden) << "the plugin's C# for coverage.proto drifted from the committed "
                                    "golden that Fletcher.Tests compiles:\n"
                                 << FirstDiff(generated, golden);
}
