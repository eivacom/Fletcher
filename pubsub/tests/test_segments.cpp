// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// PDA-DEC-A5 — topic name integrity. The seam identifies a topic by a SEGMENT
// LIST; every provider identifies it by a single JOINED BYTE STRING. These
// cases are the assertions that make that map injective and faithful:
//
//   * `SegmentsThatAliasOrTruncateAreRefused` — the four refused shapes, and
//     the accepted shapes beside them so the narrowing is bounded rather than
//     open-ended.
//   * `JoinIsInvertible` — the oracle. Over a corpus of lists, every accepted
//     list splits back to itself and no two accepted lists land on one name.
//   * `RefusalReachesAllFourEntryPoints` — the same refusal through a REAL
//     `InProcessPubSubProvider`, on all four seam methods, which is what says
//     the check lives at the one door rather than at one caller.
//   * `AcceptedNamesJoinToTheSameBytesAsBefore` — the over-reach control. It
//     is GREEN BEFORE this item and must stay green: it is what proves the
//     accepted set was narrowed without any accepted name's wire bytes moving.
//
// This file is in `pubsub_tests`, which links the IN-TREE `fletcher-pubsub`
// (`pubsub/tests/CMakeLists.txt` → `fletcher-pubsub`, built from `src/*.cpp`
// with a `BUILD_INTERFACE` include dir on the working tree). A working-tree
// edit to `internal/segments.hpp` or to `src/in_process_provider.cpp` therefore
// reaches these cases directly — unlike the conformance harness, which links
// the PACKAGED target and where such an edit is inert until a package rebuild.
// That provenance is the reason six of this item's seven mutations are cheap.

#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <fletcher/core/internal/status_name.hpp>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <functional>
#include <string>
#include <vector>

using namespace fletcher;

namespace {

using Segments = std::vector<std::string>;

/// The one byte a C string cannot carry. Spelled once, because writing `"\0"`
/// inline in a `std::string` initialiser silently yields an EMPTY string —
/// which would turn a NUL row into an empty-segment row and quietly test the
/// wrong rule.
std::string WithNul(const std::string& before, const std::string& after) {
    std::string out = before;
    out.push_back('\0');
    out += after;
    return out;
}

/// Asserts the seam refused, and refused with the seam's own number. A test
/// that merely caught "something" would be green against a `std::bad_alloc`.
::testing::AssertionResult RefusedAsInvalid(const std::function<void()>& call) {
    try {
        call();
    } catch (const PubSubError& e) {
        if (e.status() == PubSubStatus::kInvalidArgument) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure()
               << "refused with " << internal::PubSubStatusName(e.status())
               << ", wanted kInvalidArgument (" << e.what() << ")";
    } catch (const std::exception& e) {
        return ::testing::AssertionFailure()
               << "refused with an untyped exception, which §5.1 exists to replace: " << e.what();
    }
    return ::testing::AssertionFailure() << "the call was not refused at all";
}

/// The harness's own inverse of the join. Deliberately NOT product code: an
/// oracle that called a product `Split` would be comparing the product with
/// itself. Splits on every `/`, keeping empty pieces — so a trailing or
/// doubled separator is visible rather than tidied away.
Segments SplitOnSlash(const std::string& name) {
    Segments out;
    std::string current;
    for (char c : name) {
        if (c == '/') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

/// A minimal struct{x:int32}, so `CreateTopic` has something real to declare.
OwnedSchema TestSchema() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "x");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    return schema;
}

struct RefusedCase {
    Segments segments;
    const char* why;
};

/// Every shape §3.5 refuses, one row per REASON rather than one per rule, so a
/// rule that stops firing only in a later segment position still reddens.
std::vector<RefusedCase> RefusedCases() {
    return {
        {{WithNul("a", "b")}, "rule 1: a NUL truncates the name at its first byte on XRCE"},
        {{"ok", WithNul("x", "y")}, "rule 1, in a later segment"},
        {{WithNul("", "")}, "rule 1: a segment that is nothing but a NUL"},
        {{"a/b"}, "rule 2: joins onto the same name as {\"a\",\"b\"}"},
        {{"a", "b/c"}, "rule 2, in a later segment"},
        {{"/"}, "rule 2: a segment that is nothing but the separator"},
        {{""}, "rule 3: an empty segment names nothing"},
        {{"a", ""}, "rule 3: a trailing empty segment yields the name \"a/\""},
        {{"", "a"}, "rule 3: a leading empty segment yields the name \"/a\""},
        {{"a", "__schema"}, "rule 4: lands on the schema companion channel of {\"a\"}"},
        {{"__schema"}, "rule 4, as the only segment"},
        {{"__anything_at_all"}, "rule 4: the whole `__` namespace is reserved, not one name"},
        {{"__"}, "rule 4: the prefix itself"},
    };
}

/// The narrowing is bounded by these: names that work today and must keep
/// working. Dots, spaces, hyphens and a SINGLE leading underscore are not
/// wrong, and a rule that refused them would be over-reach (brief decision 3
/// rejected the safe-charset option for exactly this reason).
const std::vector<Segments>& AcceptedListCorpus() {
    static const std::vector<Segments> kLists = {
        {"a"},
        {"a", "b"},
        {"a", "b", "c"},
        {"registry", "fastdds-probe"},
        {"telemetry", "vessel.bow", "depth"},
        {"a b", "c d"},
        {"_private"},
        {"a_b", "c-d"},
        {"UPPER", "lower", "123"},
        // A literal `%`, which is accepted today. It is here so the byte table
        // below can tell REFUSING a separator from ESCAPING one: any escaping
        // scheme that is reversible must also escape its own marker, so this
        // row's bytes move under one and stand still under the other.
        {"50%", "done"},
    };
    return kLists;
}

}  // namespace

// ── THE FORCING TEST — the four refusals at the map ──────────────────
//
// Red before this item because `internal::RequireSegments` tested `segs.empty()`
// and nothing else: every row below was ACCEPTED, and three of them named a
// topic some other list also names.
TEST(Segments, SegmentsThatAliasOrTruncateAreRefused) {
    for (const RefusedCase& c : RefusedCases()) {
        EXPECT_TRUE(RefusedAsInvalid([&] {
            static_cast<void>(internal::JoinSegments(c.segments));
        })) << "JoinSegments accepted a refused shape — "
            << c.why;

        std::string out;
        EXPECT_TRUE(RefusedAsInvalid([&] { internal::JoinSegmentsInto(out, c.segments); }))
            << "JoinSegmentsInto accepted a refused shape — " << c.why;
    }

    // The empty LIST refusal is a DIFFERENT rule from the empty SEGMENT one and
    // is not absorbed by it (§3.5, PDA-DEC-3). Kept here so a refactor that
    // replaced one with the other is visible.
    EXPECT_TRUE(RefusedAsInvalid([&] { static_cast<void>(internal::JoinSegments(Segments{})); }))
        << "the empty segment list is still refused";

    // And the bound on the narrowing: everything below is still perfectly
    // ordinary. A rule that over-reached — a safe-charset whitelist, say —
    // reddens here rather than in production.
    for (const Segments& list : AcceptedListCorpus()) {
        EXPECT_NO_THROW(static_cast<void>(internal::JoinSegments(list)))
            << "an accepted name was refused: " << internal::JoinSegments(list);
    }
}

// ── THE ORACLE — the map from lists to names is injective ────────────
//
// Red before this item for a single, precise reason: `Join({"a/b"})` and
// `Join({"a","b"})` are both `a/b`, so two distinct Fletcher topics were one
// topic in every provider — the silent wrong delivery this item removes.
//
// Stated as a property over a corpus rather than as one pair, so a fix that
// only special-cased `a/b` does not satisfy it: EVERY list is either refused or
// splits back to exactly itself, and no two accepted lists share a name.
TEST(Segments, JoinIsInvertible) {
    Segments corpus_names;
    std::vector<Segments> accepted;

    std::vector<Segments> corpus = AcceptedListCorpus();
    for (const RefusedCase& c : RefusedCases()) corpus.push_back(c.segments);
    // The aliasing pair itself, named rather than left to the reader.
    corpus.push_back({"a/b"});
    corpus.push_back({"a", "b"});

    for (const Segments& list : corpus) {
        std::string name;
        try {
            name = internal::JoinSegments(list);
        } catch (const PubSubError&) {
            continue;  // refused, so it names nothing and can alias nothing
        }
        EXPECT_EQ(SplitOnSlash(name), list)
            << "an accepted list does not split back to itself: name = \"" << name << "\"";
        accepted.push_back(list);
        corpus_names.push_back(name);
    }

    for (size_t i = 0; i < corpus_names.size(); ++i) {
        for (size_t j = i + 1; j < corpus_names.size(); ++j) {
            if (accepted[i] == accepted[j]) continue;  // the same list listed twice
            EXPECT_NE(corpus_names[i], corpus_names[j])
                << "two distinct accepted lists join to one topic name: \"" << corpus_names[i]
                << "\"";
        }
    }

    // A name is bytes, and nothing on the path normalises them: these three are
    // three topics, not one. Absence of a rule, asserted, so nobody adds one.
    EXPECT_NE(internal::JoinSegments({"Topic"}), internal::JoinSegments({"topic"}));
    EXPECT_NE(internal::JoinSegments({" a"}), internal::JoinSegments({"a"}));
}

// ── The door is the DOOR — all four entry points, on a real provider ─
//
// The refusal lives in `internal::RequireSegments`, which both join functions
// call first; this is what says all four seam methods still route through it.
// It drives a REAL `InProcessPubSubProvider` rather than the join directly,
// because "the check is in the shared helper" and "every method calls the
// shared helper" are two different claims and only the second one matters.
//
// Reddens the mutation that moves the check out of `RequireSegments` into
// `CreateTopic` only — on the other three methods, in-tree, with no package
// rebuild.
TEST(Segments, RefusalReachesAllFourEntryPoints) {
    InProcessPubSubProvider provider;

    for (const RefusedCase& c : RefusedCases()) {
        const Segments& topic = c.segments;

        EXPECT_TRUE(RefusedAsInvalid([&] { provider.CreateTopic(topic, TestSchema()); }))
            << "CreateTopic accepted a refused shape — " << c.why;
        EXPECT_TRUE(RefusedAsInvalid([&] {
            provider.Publish(topic, [](WriteBuffer& buf) { buf.AppendByte(0x01); });
        })) << "Publish accepted a refused shape — "
            << c.why;
        EXPECT_TRUE(RefusedAsInvalid([&] {
            static_cast<void>(provider.Subscribe(
                topic, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
        })) << "Subscribe accepted a refused shape — "
            << c.why;
        EXPECT_TRUE(RefusedAsInvalid([&] { provider.Unsubscribe(topic); }))
            << "Unsubscribe accepted a refused shape — " << c.why;
    }

    // The positive control: an ordinary topic still passes all four. Without
    // it, a provider that refused EVERYTHING would be green above.
    const Segments ok{"segments", "door"};
    EXPECT_NO_THROW(provider.CreateTopic(ok, TestSchema()));
    EXPECT_NO_THROW(static_cast<void>(provider.Subscribe(
        ok, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {})));
    EXPECT_NO_THROW(provider.Publish(ok, [](WriteBuffer& buf) { buf.AppendByte(0x01); }));
    EXPECT_NO_THROW(provider.Unsubscribe(ok));
}

// ── THE OVER-REACH CONTROL — green today, and must stay green ────────
//
// A byte table over names that are accepted BOTH before and after this item.
// The whole item is a narrowing of the accepted set; this is what proves it is
// only that. Any design that ESCAPED a separator, percent-encoded a NUL,
// case-folded, trimmed or prefixed a namespace instead of REFUSING would move
// one of these bytes and redden here — including the two forms the providers
// DERIVE from the joined name, which are what a wire trace would actually show.
//
// Written as literals rather than by running the join twice: a table that
// compared the join with itself would assert nothing.
TEST(Segments, AcceptedNamesJoinToTheSameBytesAsBefore) {
    struct Row {
        Segments segments;
        const char* name;
    };
    const std::vector<Row> kRows = {
        {{"a"}, "a"},
        {{"a", "b"}, "a/b"},
        {{"a", "b", "c"}, "a/b/c"},
        {{"registry", "fastdds-probe"}, "registry/fastdds-probe"},
        {{"telemetry", "vessel.bow", "depth"}, "telemetry/vessel.bow/depth"},
        {{"a b", "c d"}, "a b/c d"},
        {{"_private"}, "_private"},
        {{"a_b", "c-d"}, "a_b/c-d"},
        {{"UPPER", "lower", "123"}, "UPPER/lower/123"},
        {{"50%", "done"}, "50%/done"},
    };

    for (const Row& row : kRows) {
        EXPECT_EQ(internal::JoinSegments(row.segments), std::string(row.name));

        std::string into = "stale contents, to prove the reuse path clears";
        internal::JoinSegmentsInto(into, row.segments);
        EXPECT_EQ(into, std::string(row.name))
            << "the capacity-reusing join disagrees with the allocating one";
    }

    // The two names the providers DERIVE from the joined one. Both are pure
    // functions of it (`fast_dds_pubsub_provider.cpp:331,494`,
    // `xrce_dds_pubsub_provider.cpp:720,881`; XRCE's participant name IS the
    // topic name), so pinning them here closes the last inch between "the join
    // is unchanged" and "no accepted name's wire bytes moved".
    const std::string joined = internal::JoinSegments({"telemetry", "depth"});
    EXPECT_EQ(joined + "/__schema", std::string("telemetry/depth/__schema"))
        << "the DDS schema companion name is derived from the join and must not move";
    EXPECT_EQ(joined, std::string("telemetry/depth"))
        << "the XRCE participant name IS the topic name and must not move";
}
