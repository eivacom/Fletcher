// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: shared topic segment normalisation. Used by Subscriber's
// fan-out map and FastDDSPubSubProvider's per-topic QoS map so that
// both layers key on the same string form ("a/b/c").

#ifndef FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_
#define FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_

#include <fletcher/core/status.hpp>
#include <string>
#include <vector>

namespace fletcher {
namespace internal {

// §3.5, rung 2: an empty segment list is illegal at every seam entry point. One check, in the one
// function all three providers already route every topic through — no default topic, no recovery.
inline void RequireSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "topic: an empty segment list names no topic");
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
