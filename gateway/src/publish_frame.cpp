// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "publish_frame.hpp"

#include <stdexcept>

namespace fletcher::gateway {

namespace {

uint16_t ReadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

}  // anonymous namespace

// [TOPIC_LEN:2 LE] [TOPIC:N] [ENVELOPE:rest]
PublishFrameParts ParsePublishFrame(const uint8_t* data, std::size_t len) {
    if (len < 2) {
        throw std::invalid_argument("publish: frame too short for TOPIC_LEN");
    }
    const uint16_t topic_len = ReadU16LE(data);
    if (static_cast<std::size_t>(2) + topic_len > len) {
        throw std::invalid_argument("publish: truncated topic");
    }

    PublishFrameParts parts;
    parts.topic.assign(reinterpret_cast<const char*>(data + 2), topic_len);
    parts.envelope_data = data + 2 + topic_len;
    parts.envelope_size = len - 2 - topic_len;
    return parts;
}

}  // namespace fletcher::gateway
