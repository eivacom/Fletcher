// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: the seam's topic-name map. Used by Subscriber's fan-out map and
// FastDDSPubSubProvider's per-topic QoS map so that both layers key on the same
// string form ("a/b/c") — and, since PDA-DEC-A5, the one place that makes that
// map injective and faithful. The join is the SEAM's, not a provider's
// discretion (§3.5): the name computed here IS the topic's identity, and a
// driver may map it into its own transport namespace only injectively.

#ifndef FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_
#define FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_

#include <fletcher/core/status.hpp>
#include <string>
#include <vector>

namespace fletcher {
namespace internal {

/// §3.5, rung 2 — the gate that makes the segment list the topic's identity.
///
/// The seam identifies a topic by a SEGMENT LIST; every provider identifies it by the single
/// `/`-joined byte string produced below. This function is what makes that map trustworthy, and
/// it is the ONE door: both joins call it first, and all twelve provider entry points plus the
/// caller tier, `pubsub-arrow` and the conformance peer reach a topic only through a join.
///
/// The invariant it establishes, for every accepted list `L`:
///
///   * `Join(L)` contains no NUL, so the name each provider hands its transport is the WHOLE
///     name — XRCE passes it to `uxr_buffer_create_topic_bin`/`..._participant_bin` as a
///     `const char*`, which has no length form, so a zero byte there is silent truncation that
///     cannot be repaired at the sink;
///   * `Split(Join(L)) == L`, so two distinct accepted lists are two distinct topics in EVERY
///     provider — `{"a/b"}` and `{"a","b"}` used to be one;
///   * `Join(L)` is not a name a provider DERIVES, because the `__` namespace those companions
///     live in (`name + "/__schema"` in both DDS providers) is reserved.
///
/// Refused, all with `kInvalidArgument` — no new status; only the owner allocates those:
///
///   1. the empty LIST — there is no default topic and no recovery (PDA-DEC-3);
///   2. a segment containing a NUL;
///   3. a segment containing `/`;
///   4. an EMPTY segment — §3.5's empty-list rule one level down: `{""}` reproduces the very
///      name rule 1 forbids, and `{"a",""}` names `"a/"`;
///   5. a segment beginning `__` — the PREFIX, not the literal `__schema`, so every present and
///      future provider-derived companion name is out of reach without a further ruling
///      (owner ruling 2026-09-04).
///
/// Deliberately absent: trimming, case folding, Unicode normalisation, escaping. Identity is
/// bytes. There is no normalisation step anywhere on this path, so there is no place for two
/// providers to disagree about what a name means — the same words §4's selector rule uses, so a
/// language binding learns ONE rule.
///
/// Cost: `JoinSegmentsInto` runs per sample, so this is one linear pass over bytes the join is
/// about to copy anyway — no allocation, no `find_first_of` with a constructed needle. It is
/// unconditional at every entry point: validating only in `CreateTopic` would be a partial mode,
/// and `Publish` to an undeclared topic is reachable.
inline void RequireSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "topic: an empty segment list names no topic");
    }
    for (const std::string& seg : segs) {
        if (seg.empty()) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "topic: an empty segment names nothing");
        }
        if (seg.size() >= 2 && seg[0] == '_' && seg[1] == '_') {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "topic: segments beginning \"__\" are reserved for provider-derived "
                              "companion channels: " +
                                  seg);
        }
        for (char c : seg) {
            if (c == '/') {
                throw PubSubError(PubSubStatus::kInvalidArgument,
                                  "topic: a segment may not contain the separator '/', which "
                                  "would name a different segment list: " +
                                      seg);
            }
            if (c == '\0') {
                throw PubSubError(PubSubStatus::kInvalidArgument,
                                  "topic: a segment may not contain a zero byte, which would "
                                  "truncate the name on the wire");
            }
        }
    }
}

// Joins into `out`, reusing its capacity. For the publish path, where building a fresh std::string
// to index the topic map was a malloc and a free on every sample.
//
// Deliberately not shared with JoinSegments below: routing that through here made it start from an
// empty string where it used to copy-construct from segs[0], which costs a reallocation and showed
// up as ~2.5% on bench_pubsub_fanout's BM_CreateTopic_Redeclare. Six duplicated lines are cheaper.
inline void JoinSegmentsInto(std::string& out, const std::vector<std::string>& segs) {
    RequireSegments(segs);
    out.clear();
    out += segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
}

inline std::string JoinSegments(const std::vector<std::string>& segs) {
    RequireSegments(segs);
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_
