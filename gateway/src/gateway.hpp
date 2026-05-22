// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Internal header for the `gateway` executable. Not part of any
// installed/published interface — gateway is distributed as a binary
// only. The WebSocket protocol is the supported integration point.
#ifndef FLETCHER_GATEWAY_GATEWAY_HPP_
#define FLETCHER_GATEWAY_GATEWAY_HPP_

#include <cstdint>
#include <fletcher/pubsub/driver.hpp>
#include <memory>
#include <string>

namespace fletcher {

struct GatewayOptions {
    std::string address = "0.0.0.0";
    uint16_t port = 9090;
    int io_threads = 1;
};

/// WebSocket server that exposes a Driver over the network. Clients
/// connect via WebSocket and exchange JSON control frames (subscribe,
/// unsubscribe, publish, list_topics) and binary data frames
/// (serialised Envelope bytes).
class Gateway {
   public:
    explicit Gateway(std::shared_ptr<Driver> driver, GatewayOptions options = {});
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    /// Start listening for connections.  Non-blocking — spins up IO
    /// threads internally.
    void Start();

    /// Stop the IO context, join IO threads, and release resources.
    void Stop();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_GATEWAY_GATEWAY_HPP_
