// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// fletcher-gateway — Fletcher's WebSocket gateway server.
//
// Distributed as a single executable. The only supported integration
// point is the WebSocket protocol — there is no public C++ API.
//
//   gateway --port 9090 --bind-address 0.0.0.0
//
// CLI:
//   --port N               TCP port to listen on (default 9090).
//   --bind-address ADDR    bind address (default 0.0.0.0).
//   --provider NAME        pub/sub provider (default "inprocess"). This file
//                          registers exactly two names, "inprocess" and
//                          "fastdds" — but that list lives in the registry
//                          below, NOT here: an unrecognised NAME is refused
//                          at startup (exit 2) naming what IS registered, so
//                          this comment is not the authority a third
//                          provider would have to keep in sync.
//   --domain-id N          DDS domain id for the fastdds provider (default 0).
//                          Ignored by the inprocess provider.
//
// The gateway is schema-agnostic. It knows nothing about topic schemas
// or which topics exist before clients show up; clients establish
// topics by subscribing or publishing and supply their own schemas
// (typically generated from a `.proto` by protoc-gen-fletcher).
//
// Process lifecycle:
//   * Prints "READY <port>" on stdout once the gateway is accepting
//     connections so launchers can synchronise without polling the
//     socket.
//   * Reads stdin and exits cleanly on the literal line "stop".
//     Deterministic shutdown without relying on SIGTERM ordering
//     (Windows SIGTERM semantics differ from POSIX).
//
// Provider note:
//   The default ("inprocess") is a loopback that only connects WebSocket
//   clients on the same process. This file also registers a DDS-backed
//   provider under "fastdds", which bridges the gateway to any FastDDS app on
//   the same DDS domain, so a WebSocket client can pub/sub to data flowing
//   over DDS. Both are always compiled into the exe, registered
//   unconditionally, and selected at runtime by ONE registry lookup (spec
//   §4) — this file keeps no separate list of valid names, and neither
//   should a reader of this comment: the registry's own refusal, not this
//   text, is what says which providers a given build has.

#include <cstdio>
#include <cstdlib>
#include <fletcher/core/status.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "gateway.hpp"

namespace {

struct Args {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 9090;
    std::string provider = "inprocess";
    uint32_t domain_id = 0;
};

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bind-address" && i + 1 < argc) {
            a.bind_address = argv[++i];
        } else if (arg == "--provider" && i + 1 < argc) {
            a.provider = argv[++i];
        } else if (arg == "--domain-id" && i + 1 < argc) {
            a.domain_id = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--version") {
            std::printf("fletcher-gateway %s\n", GATEWAY_VERSION_STRING);
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            // "--provider NAME", not an enumerated grammar: the registry
            // below is the single list, and an unrecognised NAME is refused
            // at startup naming what IS registered, so this usage line does
            // not duplicate it (rung-1 forbidden case 4).
            // Nor is the list derived from the registry, and it must not be:
            // once a path resolver is installed, an enumeration can only ever
            // report BUILT-INS, which would hand code above the seam a way to
            // tell built-in from loaded (decision 3). The registry's surface is
            // frozen at Create/Register/SetPathResolver "and nothing else"
            // (provider_registry.hpp) — a name-listing accessor is not a gap.
            std::printf(
                "Usage: %s [--port N] [--bind-address ADDR] "
                "[--provider NAME] [--domain-id N] [--version]\n"
                "  --provider defaults to \"inprocess\"; an unrecognised NAME "
                "exits 2 naming what this build supports.\n",
                argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "fletcher-gateway: unknown argument: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    // No provider-name validation here: the registry below is the single list
    // (spec §4), and it refuses an unknown selector itself, naming what IS
    // registered.
    return a;
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fletcher-gateway: bad CLI: %s\n", e.what());
        return 2;
    }

    std::shared_ptr<fletcher::PubSubProvider> provider;
    try {
        // DEBT-4 disclosure: this `try` is new. Before this item a Fast DDS
        // construction failure was an uncaught exception escaping `main` (a
        // crash/abort), not a clean exit — this one `catch` now turns it into
        // the same "exit 2 plus a message" every other provider refusal gets.
        // An improvement, and consistent with §5.1, but it is NOT the
        // "observably unchanged" this item's other half claims to be; said
        // here because it is not said anywhere else durable.
        //
        // Both built-ins are registered UNCONDITIONALLY and before the
        // selector is looked at: registration states availability (a
        // link-time fact), `Create` performs selection (a runtime string).
        // Branching registration on `args.provider` would put a selector
        // branch back above the seam — exactly what locked decision 3
        // forbids. The `fastdds` factory's closure is not called unless
        // "fastdds" is selected, so an inprocess run costs exactly what it
        // costs today.
        fletcher::ProviderRegistry registry;
        fletcher::RegisterInProcessProvider(registry);
        registry.Register("fastdds", [](const fletcher::ProviderConfig& c) {
            fletcher::FastDDSProviderOptions dds_opts;
            dds_opts.domain_id = c.domain_id;
            return std::make_shared<fletcher::FastDDSPubSubProvider>(std::move(dds_opts));
        });

        // DEBT-5 (PDA-DEC-5): the gateway has no CLI route for a provider
        // document, so `document` is always empty here. That is correct for
        // `inprocess` (its default IS today's behaviour), but a later stage
        // moving Fast DDS QoS into the document (PDA-DEC-6) needs a way for an
        // operator to supply one — no item currently owns adding that surface.
        fletcher::ProviderConfig config;
        config.domain_id = args.domain_id;
        provider = registry.Create(fletcher::ProviderSelector::Parse(args.provider), config);
    } catch (const fletcher::PubSubError& e) {
        std::fprintf(stderr, "fletcher-gateway: %s\n", e.what());
        return 2;
    }

    auto publisher = std::make_shared<fletcher::Publisher>(provider);
    auto subscriber = std::make_shared<fletcher::Subscriber>(provider);

    fletcher::GatewayOptions opts;
    opts.address = args.bind_address;
    opts.port = args.port;

    fletcher::Gateway gw(std::move(publisher), std::move(subscriber), opts);
    gw.Start();

    std::printf("READY %u\n", args.port);
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }

    gw.Stop();
    return 0;
}
