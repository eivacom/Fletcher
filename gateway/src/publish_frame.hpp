// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Parser for the gateway's binary PUBLISH frame.
//
//   [TOPIC_LEN :2 LE][TOPIC :N bytes][ENVELOPE :rest of frame]
//
// Internal to the gateway exe — not part of any public include path.
// Lives in its own translation unit so unit tests can exercise the
// validation edge cases without standing up a Boost.Beast WebSocket.

#ifndef FLETCHER_GATEWAY_PUBLISH_FRAME_HPP_
#define FLETCHER_GATEWAY_PUBLISH_FRAME_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

namespace fletcher::gateway {

/// Result of a successful PUBLISH frame parse. `envelope_data` and
/// `envelope_size` reference the trailing bytes of the input frame
/// the parser was given — the caller must not free the input before
/// using these.
struct PublishFrameParts {
    std::string    topic;
    const uint8_t* envelope_data;
    std::size_t    envelope_size;
};

/// Parse a PUBLISH binary frame. Throws `std::invalid_argument` when
/// the frame is too short to contain a topic length, when the
/// declared topic length runs off the end of the frame, or when no
/// envelope bytes follow the topic.
PublishFrameParts ParsePublishFrame(const uint8_t* data, std::size_t len);

}  // namespace fletcher::gateway

#endif  // FLETCHER_GATEWAY_PUBLISH_FRAME_HPP_
