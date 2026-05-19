// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "gateway.hpp"
#include "ws_session.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>

#include <thread>
#include <vector>

namespace fletcher {

namespace net = boost::asio;
using tcp     = net::ip::tcp;

// -----------------------------------------------------------------------
// Listener — accepts TCP connections and spawns WsSessions.
// -----------------------------------------------------------------------

class Listener : public std::enable_shared_from_this<Listener> {
 public:
    Listener(net::io_context& ioc,
             tcp::endpoint endpoint,
             std::shared_ptr<Driver> driver)
        : ioc_(ioc)
        , acceptor_(net::make_strand(ioc))
        , driver_(std::move(driver)) {
        beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) throw std::runtime_error("Listener open: " + ec.message());

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) throw std::runtime_error("Listener reuse_address: " + ec.message());

        acceptor_.bind(endpoint, ec);
        if (ec) throw std::runtime_error("Listener bind: " + ec.message());

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) throw std::runtime_error("Listener listen: " + ec.message());
    }

    void Run() { DoAccept(); }

 private:
    void DoAccept() {
        auto self = shared_from_this();
        acceptor_.async_accept(
            net::make_strand(ioc_),
            [self](beast::error_code ec, tcp::socket socket) {
                if (ec) return;  // acceptor was closed (Stop)
                std::make_shared<WsSession>(
                    std::move(socket), self->driver_)->Run();
                self->DoAccept();
            });
    }

    net::io_context&        ioc_;
    tcp::acceptor           acceptor_;
    std::shared_ptr<Driver> driver_;
};

// -----------------------------------------------------------------------
// Gateway::Impl
// -----------------------------------------------------------------------

struct Gateway::Impl {
    GatewayOptions            options;
    std::shared_ptr<Driver>      driver;
    std::unique_ptr<net::io_context> ioc;
    std::shared_ptr<Listener>    listener;
    std::vector<std::thread>     threads;
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

Gateway::Gateway(std::shared_ptr<Driver> driver,
                       GatewayOptions options)
    : impl_(std::make_unique<Impl>()) {
    if (!driver)
        throw std::invalid_argument("Gateway: driver must not be null");
    impl_->driver  = std::move(driver);
    impl_->options = std::move(options);
}

Gateway::~Gateway() {
    Stop();
}

// -----------------------------------------------------------------------
// Start / Stop
// -----------------------------------------------------------------------

void Gateway::Start() {
    if (impl_->ioc) return;  // already started

    auto& opts = impl_->options;
    int n = std::max(1, opts.io_threads);
    impl_->ioc = std::make_unique<net::io_context>(n);

    auto ep = tcp::endpoint(
        net::ip::make_address(opts.address), opts.port);

    impl_->listener = std::make_shared<Listener>(
        *impl_->ioc, ep, impl_->driver);
    impl_->listener->Run();

    // Spin up IO threads.
    impl_->threads.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        impl_->threads.emplace_back([ioc = impl_->ioc.get()] {
            ioc->run();
        });
    }
}

void Gateway::Stop() {
    if (!impl_->ioc) return;

    impl_->ioc->stop();
    for (auto& t : impl_->threads) {
        if (t.joinable()) t.join();
    }
    impl_->threads.clear();
    impl_->listener.reset();
    impl_->ioc.reset();
}

}  // namespace fletcher
