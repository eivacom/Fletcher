// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal: shared topic segment normalisation. Used by Subscriber's
// fan-out map and FastDDSPubSubProvider's per-topic QoS map so that
// both layers key on the same string form ("a/b/c").

#ifndef FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_
#define FLETCHER_INCLUDE_PUBSUB_INTERNAL_SEGMENTS_HPP_

#include <string>
#include <vector>

namespace fletcher {
namespace internal {

inline std::string JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) {
        return {};
    }
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
