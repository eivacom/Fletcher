// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The published error taxonomy cannot drift from the enum that defines it (PDA-DEC-9, spec
// §5.1 + §12 condition 3).
//
// Two independent language bindings need the STATUS NUMBERS in prose, and this file is what
// makes the one published copy — the `Error taxonomy (published)` table in `core/README.md` —
// unable to disagree with `PubSubStatus`. Four parts, four different mutations:
//
//   1. `StatusName` is one `switch` over every enumerator with NO `default:` label, compiled
//      with the unhandled-enumerator diagnostic promoted to an error on this one source file
//      (`/we4062` on MSVC, `-Werror=switch` on GCC/Clang; see tests/CMakeLists.txt).
//      APPENDING AN ENUMERATOR THEREFORE FAILS THE BUILD, not the test. The trailing
//      `return ""` is a statement AFTER the switch, not a `default:` label, so it keeps MSVC
//      C4715 quiet without suppressing C4062.
//   2. Row for row against the file: `StatusName(cast(number)) == name`, so editing a name or
//      a number on EITHER side goes red.
//   3. One past the last row is not a status: `StatusName(cast(rows.size()))` must be empty.
//      Part 1 forces an appender into this file; part 3 then holds the suite red until the
//      README carries the new row.
//   4. A published row the parser cannot read is a FAILURE, not a skipped line: only the
//      table's header and separator lines are skipped, and they are skipped STRUCTURALLY (a
//      markdown table has exactly those two). So an ADDED row whose `Number` cell is `TBD`, a
//      typo, or outside `int` range reddens instead of dropping out of the comparison
//      unnoticed — a row the parser silently drops is a published status the enum is never
//      compared against.
//
// THIS TEST HOLDS NO COUNT AND NO COPY OF THE NUMBERS. A row-count equality against a number
// the test itself carries would BE the held-copy defect the guard exists to close (design F3),
// so the expected set is derived from the file: the published numbers are asserted to be
// exactly `0 .. rows.size()-1` **in the order published**, which spec §5.1's "appended only,
// never renumbered, reordered or reused" is what makes legitimate.
//
// The README is read at RUN time, off disk, by absolute path from
// `target_compile_definitions` — the tree's convention for a test that reads a
// source-controlled artefact (precedent: `xrcedds-pubsub-provider/tests/CMakeLists.txt:18-24`
// and `test_xrce_document.cpp`'s `PublishedDefaultsAreExact`). A `file(READ)`/`configure_file`
// bake-in at configure time would re-create the very held-copy defect the disk read kills.

#include <gtest/gtest.h>

#include <cstdint>
#include <exception>
#include <fletcher/core/status.hpp>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using fletcher::PubSubStatus;

namespace {

// EXHAUSTIVE, and no `default:` label — that absence is the guard (see part 1 above).
std::string StatusName(PubSubStatus status) {
    switch (status) {
        case PubSubStatus::kOk:
            return "kOk";
        case PubSubStatus::kInvalidArgument:
            return "kInvalidArgument";
        case PubSubStatus::kSchemaConflict:
            return "kSchemaConflict";
        case PubSubStatus::kTopicNotDeclared:
            return "kTopicNotDeclared";
        case PubSubStatus::kPayloadTooLarge:
            return "kPayloadTooLarge";
        case PubSubStatus::kTransportFailure:
            return "kTransportFailure";
        case PubSubStatus::kNotSupported:
            return "kNotSupported";
        case PubSubStatus::kInternal:
            return "kInternal";
        case PubSubStatus::kPending:
            return "kPending";
        case PubSubStatus::kSubscriptionEnded:
            return "kSubscriptionEnded";
    }
    return "";  // a statement, not a `default:` label
}

#ifndef FLETCHER_CORE_README_PATH
#error "FLETCHER_CORE_README_PATH is not defined; see tests/CMakeLists.txt"
#endif
constexpr const char* kReadmePath = FLETCHER_CORE_README_PATH;

// The heading the published table sits under. A renamed heading parses zero rows, which the
// caller asserts on — never a silently skipped loop.
constexpr const char* kTableMarker = "## Error taxonomy (published)";

// Empty on an unreadable or missing file; the caller asserts, exactly as the precedent does.
std::string ReadWholeFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Trims whitespace and markdown code ticks, so `` `kOk` `` in the table compares against the
// enumerator spelling.
std::string Trim(const std::string& text) {
    constexpr const char* kBlank = " \t\r\n`";
    const size_t first = text.find_first_not_of(kBlank);
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(kBlank);
    return text.substr(first, last - first + 1);
}

struct PublishedRow {
    std::string name;
    int32_t number = 0;
};

// `| a | b |` split into trimmed cells, without the empty artefacts the leading and trailing
// pipes produce.
std::vector<std::string> SplitCells(const std::string& table_line) {
    std::vector<std::string> cells;
    std::istringstream cell_stream(table_line);
    std::string cell;
    while (std::getline(cell_stream, cell, '|')) cells.push_back(Trim(cell));
    while (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
    while (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

// The first markdown table after `marker`, bounded to that section: every `|`-led line until
// the first line that is not one. The header and `|---|` separator lines are skipped
// STRUCTURALLY — a markdown table has exactly those two, in that order — and every remaining
// line must yield a name and a whole number or it is a FAILURE (part 4). Nothing here holds an
// expected row count.
std::vector<PublishedRow> ParsePublishedRows(const std::string& text, const std::string& marker) {
    std::vector<PublishedRow> rows;
    const size_t at = text.find(marker);
    if (at == std::string::npos) return rows;

    // Bounded to this section: if the table is deleted, a later table elsewhere in the file
    // must not be able to answer in its place.
    const size_t body_at = at + marker.size();
    const size_t next_heading = text.find("\n## ", body_at);
    const std::string section = text.substr(
        body_at, next_heading == std::string::npos ? std::string::npos : next_heading - body_at);

    std::istringstream lines(section);
    std::string line;
    bool in_table = false;
    size_t table_line_index = 0;
    while (std::getline(lines, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() != '|') {
            if (in_table) break;  // the table ended
            continue;             // prose between the heading and the table
        }
        in_table = true;

        const size_t index = table_line_index++;
        if (index == 0) continue;  // the header line, skipped structurally
        if (index == 1) {          // the separator line, likewise
            if (trimmed.find_first_not_of("|:- ") != std::string::npos) {
                ADD_FAILURE() << "expected a markdown separator line as the published table's "
                                 "second line, got: "
                              << trimmed;
            }
            continue;
        }

        const std::vector<std::string> cells = SplitCells(trimmed);
        if (cells.size() < 2 || cells[0].empty()) {
            ADD_FAILURE() << "published taxonomy row has no Name and Number cells: " << trimmed;
            continue;
        }
        size_t consumed = 0;
        int parsed = 0;
        try {
            parsed = std::stoi(cells[1], &consumed);
        } catch (const std::exception&) {
            consumed = 0;  // not a number at all, or outside int range
        }
        if (cells[1].empty() || consumed != cells[1].size()) {
            ADD_FAILURE() << "published taxonomy row `" << cells[0]
                          << "` does not carry a whole number in its Number cell: " << trimmed;
            continue;
        }
        rows.push_back(PublishedRow{cells[0], static_cast<int32_t>(parsed)});
    }
    return rows;
}

}  // namespace

// Spec §12 condition 3's machine check. Reddens on four different mutations; see the header
// comment for which mutation hits which part.
TEST(Taxonomy, PublishedNumbersMatchTheEnum) {
    const std::string readme = ReadWholeFile(kReadmePath);
    ASSERT_FALSE(readme.empty()) << "could not read the published taxonomy from " << kReadmePath;

    const std::vector<PublishedRow> rows = ParsePublishedRows(readme, kTableMarker);
    ASSERT_FALSE(rows.empty()) << "no taxonomy rows parsed under '" << kTableMarker << "' in "
                               << kReadmePath
                               << " — the table is the enumeration, so a missing or renamed "
                                  "section is a failure, not an empty pass";

    // Contiguity, derived rather than held: the published numbers are exactly 0..rows.size()-1
    // in the order they are published (spec §5.1 — appended only, never renumbered, reordered
    // or reused). This is what lets part 3 below ask about "one past the last" without the test
    // carrying a count.
    std::vector<int32_t> numbers;
    numbers.reserve(rows.size());
    for (const PublishedRow& row : rows) numbers.push_back(row.number);
    std::vector<int32_t> contiguous(rows.size());
    std::iota(contiguous.begin(), contiguous.end(), 0);
    EXPECT_EQ(numbers, contiguous) << "the published numbers are not 0..n-1 in the order published";

    // Part 2: row for row. A name or a number edited on either side goes red.
    for (const PublishedRow& row : rows) {
        EXPECT_EQ(StatusName(static_cast<PubSubStatus>(row.number)), row.name)
            << "published row " << row.number << " (" << row.name
            << ") does not match PubSubStatus";
    }

    // Part 3: one past the last published row is not a status. With part 1 making an append a
    // COMPILE failure, this is what keeps the suite red until the README gains the row.
    EXPECT_EQ(StatusName(static_cast<PubSubStatus>(static_cast<int32_t>(rows.size()))), "")
        << "an enumerator exists one past the last published row: it was added to the enum and "
           "to StatusName, but not to "
        << kReadmePath;
}
