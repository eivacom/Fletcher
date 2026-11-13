// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BC-9's scan guard (d9 L15; D9S-4a design, "Guards (b)"). There is ONE batch assembler and it is
// domain 7's. The shim hands over what that assembler closes: it keeps no window, counts no rows,
// owns no timer and starts no thread of its own. A shim that grew one would be a second assembler
// beside the one the owner ruled to be the only one (ruling 2026-11-12, "C# moves onto the C++
// batcher").
//
// The scan is the cheapest proof that can fail: every file under `c-abi/src` is searched for the
// names a thread, a clock, a sleep or a wait would have to carry. It is a plain substring match, on
// comments too, on purpose: a source that wants to say one of these words says something else, and
// no stripping logic exists to be wrong. The patterns and the directory are the whole rule.
//
// Compiled into `c_abi_tests` only. The directory is passed by `tests/CMakeLists.txt` as one
// forward-slash literal (CMake spells its source dir that way on every platform), so the same
// string is a path on Windows and Linux.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef FLETCHER_C_ABI_SRC_DIR
#error "FLETCHER_C_ABI_SRC_DIR is not defined: tests/CMakeLists.txt passes the shim's src directory as a forward-slash literal."
#endif

namespace {

namespace fs = std::filesystem;

/// What a shim that batched, timed or threaded would have to name: a standard thread, a raw OS
/// thread, a clock, a sleep, a timed wait, a future, and the OS timer APIs. A closed deny-list, so
/// it is exactly as wide as the ways this code base could start a thread or arm a timer.
const std::vector<std::string>& ForbiddenPatterns() {
    static const std::vector<std::string> patterns = {
        "std::thread",  "jthread",    "pthread_create", "CreateThread",
        "_beginthread", "condition_variable", "_clock", "sleep_for",
        "sleep_until",  "wait_for",   "wait_until",     "std::future",
        "std::async",   "timer_create", "SetTimer",     "CreateTimerQueueTimer"};
    return patterns;
}

/// The matcher: which of the forbidden patterns `text` contains, in the list's order.
std::vector<std::string> PatternsIn(const std::string& text) {
    std::vector<std::string> hits;
    for (const std::string& pattern : ForbiddenPatterns()) {
        if (text.find(pattern) != std::string::npos) hits.push_back(pattern);
    }
    return hits;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

bool IsSourceFile(const fs::path& path) {
    const std::string ext = path.extension().string();
    return ext == ".cpp" || ext == ".hpp" || ext == ".c" || ext == ".h" || ext == ".cc";
}

TEST(ShimBatchesNothing, SourcesHoldNoThreadClockOrTimer) {
    const fs::path dir(FLETCHER_C_ABI_SRC_DIR);
    ASSERT_TRUE(fs::is_directory(dir)) << "the shim's src directory is not at " << dir.string();

    size_t scanned = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || !IsSourceFile(entry.path())) continue;
        ++scanned;
        const std::string text = ReadAll(entry.path());
        EXPECT_FALSE(text.empty()) << entry.path().string() << " could not be read";
        for (const std::string& hit : PatternsIn(text)) {
            ADD_FAILURE() << entry.path().filename().string() << " names '" << hit
                          << "': the shim batches nothing, owns no thread, clock or timer (BC-9, "
                             "d9 L15). Domain 7 closes windows and delivers them.";
        }
    }
    EXPECT_GT(scanned, 0U) << "the scan found no source under " << dir.string()
                           << ", so it proved nothing";
}

/// The control: the scan is only worth anything if its matcher can say yes. This runs the SAME
/// matcher over this test's own source, which names every pattern in its list, and requires every
/// one to hit. A matcher that matched nothing, or only some, would let the guard above pass on a
/// shim that had a thread.
TEST(ShimBatchesNothing, TheMatcherFindsEveryPatternItIsGivenInItsOwnSource) {
    const std::string self = ReadAll(fs::path(__FILE__));
    ASSERT_FALSE(self.empty()) << "could not read this test's own source at " << __FILE__;

    const std::vector<std::string> hits = PatternsIn(self);
    for (const std::string& pattern : ForbiddenPatterns()) {
        EXPECT_NE(std::find(hits.begin(), hits.end(), pattern), hits.end())
            << "the matcher did not find '" << pattern << "' in text that contains it";
    }
    EXPECT_EQ(hits.size(), ForbiddenPatterns().size());
}

}  // namespace
