#ifndef FLETCHER_INCLUDE_WEB_GATEWAY_HPP_
#define FLETCHER_INCLUDE_WEB_GATEWAY_HPP_

#include <pubsub/driver.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace fletcher {

struct WebGatewayOptions {
    std::string address    = "0.0.0.0";
    uint16_t    port       = 9090;
    int         io_threads = 1;
};

/// WebSocket server that exposes a Driver over the network.
///
/// Clients connect via WebSocket and exchange JSON control frames
/// (subscribe, unsubscribe, publish, list_topics) and binary data
/// frames (serialised Envelope bytes).
class WebGateway {
 public:
    explicit WebGateway(std::shared_ptr<Driver> driver,
                        WebGatewayOptions options = {});
    ~WebGateway();

    WebGateway(const WebGateway&) = delete;
    WebGateway& operator=(const WebGateway&) = delete;

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

#endif  // FLETCHER_INCLUDE_WEB_GATEWAY_HPP_
