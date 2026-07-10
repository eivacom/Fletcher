// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-7 forcing test: TsVisitor.DescriptorByteIdentical.
//
// The TypeScript interface + runtime TypedSchema/SchemaDescriptor is now produced
// by a direct recursive IR visitor (ts_backend::TsVisitor) instead of the flat
// FieldInfo/FieldMapping switches. This test is the byte-identity gate for that
// cutover: the .fletcher.ts the migrated plugin emits for the coverage fixture
// (GENERATED_TS_PATH, written by the real protoc run wired in CMakeLists) must be
// byte-for-byte identical to the COMMITTED frozen golden (TS_GOLDEN_PATH), which
// was captured from the pre-migration emitter.
//
// The golden is a source-controlled file, never a build artifact, and ctest never
// rewrites it. To (re)baseline after a reviewed semantic-parity change, run with
// FLETCHER_REGEN_GOLDEN=1 (which copies the freshly generated file over the golden
// and then reports the test as skipped) — a byte change for an already-supported
// input is otherwise a stop-and-ask (locked decision #1/#2).
//
// The parked scalar-leaf flatten cases (coverage_future.proto) are intentionally
// NOT exercised here; GIR-10 owns those.

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
        bool ga = static_cast<bool>(std::getline(sa, la));
        bool gb = static_cast<bool>(std::getline(sb, lb));
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

TEST(TsVisitor, DescriptorByteIdentical) {
    const std::string generated_path = GENERATED_TS_PATH;
    const std::string golden_path = TS_GOLDEN_PATH;

    const std::string generated = ReadFile(generated_path);
    ASSERT_FALSE(generated.empty())
        << "generated TS not found or empty: " << generated_path
        << " (was the coverage plugin run with --fletcher_opt=ts?)";

    // Gated regeneration: overwrite the committed golden and skip. Never runs in
    // normal ctest.
    if (const char* regen = std::getenv("FLETCHER_REGEN_GOLDEN"); regen && *regen) {
        std::ofstream out(golden_path, std::ios::binary);
        out << generated;
        GTEST_SKIP() << "FLETCHER_REGEN_GOLDEN set: rewrote golden " << golden_path;
    }

    const std::string golden = ReadFile(golden_path);
    ASSERT_FALSE(golden.empty())
        << "committed golden not found or empty: " << golden_path;

    EXPECT_EQ(generated, golden)
        << "IR-driven TS drifted from the committed golden for the coverage "
           "fixture (byte-identity break — stop-and-ask per locked #1/#2).\n"
        << FirstDiff(generated, golden);
}
