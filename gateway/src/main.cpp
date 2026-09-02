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
//   --provider-config FILE read FILE and hand its contents to the selected
//                          provider as its configuration document. The format
//                          is the PROVIDER's, not the gateway's: a Fast DDS XML
//                          QoS profiles document for "fastdds", `key=value`
//                          lines for "inprocess". The gateway does not parse it,
//                          does not validate it and does not know what it means
//                          — it only reads the bytes, because Fletcher never
//                          opens a file on a provider's behalf (owner ruling
//                          2026-09-02: the configuration setting carries the
//                          document itself, and the convenience of reading a
//                          file lives HERE). An unreadable FILE exits 2, like a
//                          bad selector; a document the provider rejects exits 2
//                          with the provider's own message.
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
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#include "gateway.hpp"

namespace {

struct Args {
    std::string bind_address = "0.0.0.0";
    uint16_t port = 9090;
    std::string provider = "inprocess";
    uint32_t domain_id = 0;
    // The CONTENTS of --provider-config, not its path: what crosses the seam is
    // the document itself (§4.1), so the file is read here and the name is not
    // kept. Empty means "no document", which is every provider's own default.
    std::string document;
};

// Read a provider document off disk. This is the ONLY file the gateway opens on
// a provider's behalf, and it is opened in BINARY: the document is opaque bytes
// (§4.2, C form: "the bytes may contain NUL"), so no newline translation and no
// text-mode truncation at a stray 0x1A.
std::string ReadProviderDocument(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "fletcher-gateway: cannot read --provider-config %s\n", path);
        std::exit(2);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad()) {
        std::fprintf(stderr, "fletcher-gateway: error reading --provider-config %s\n", path);
        std::exit(2);
    }
    std::string document = buffer.str();

    // An EMPTY file is refused, and with its own message. Every provider reads an empty document
    // as "my own defaults", so a zero-length, truncated or wrong-but-empty file would otherwise
    // start a gateway that applies none of the operator's intent and says nothing at all. Whoever
    // passed --provider-config asked to be configured FROM THAT FILE; an empty read fails that
    // request as squarely as an unreadable one, and this is the only place that can tell "no flag"
    // from "flag, empty file" (review 4b S2, and the item's own rule: refuse at start-up so a
    // misconfigured instance never exists). Whitespace-only counts as empty: it is what an editor
    // leaves behind, and no provider's format has a meaning for it.
    if (document.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
        std::fprintf(stderr,
                     "fletcher-gateway: --provider-config %s is empty; omit the flag to run on "
                     "the provider's own defaults\n",
                     path);
        std::exit(2);
    }
    return document;
}

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    // The path is remembered and read AFTER the flag loop, so `--provider-config missing.xml
    // --help` prints help instead of exiting 2 on a file the user never meant to use.
    const char* document_path = nullptr;
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
        } else if (arg == "--provider-config" && i + 1 < argc) {
            document_path = argv[++i];
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
                "[--provider NAME] [--domain-id N] [--provider-config FILE] "
                "[--version]\n"
                "  --provider defaults to \"inprocess\"; an unrecognised NAME "
                "exits 2 naming what this build supports.\n"
                "  --provider-config FILE is passed to the provider verbatim, "
                "in the provider's own format.\n",
                argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "fletcher-gateway: unknown argument: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    if (document_path) a.document = ReadProviderDocument(document_path);
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
        // PDA-DEC-6: the provider registers itself, so this file no longer names
        // a concrete provider type at all — the last one went with the inline
        // closure that used to translate `ProviderConfig` into a Fast
        // DDS-specific options struct. Nothing here knows any DDS vocabulary.
        fletcher::RegisterFastDDSProvider(registry);

        // PDA-DEC-5 DEBT-5 is CLOSED here: `--provider-config FILE` is the route
        // an operator uses to configure the selected provider, which is what
        // makes charter requirement (b) — "there is a way for me to configure
        // the driver with protocol-specific setup details at runtime" —
        // reachable from gateway.exe. The gateway supplies bytes and nothing
        // else: it neither knows nor checks the format, so ONE flag serves every
        // provider this build has and every provider a later build adds.
        fletcher::ProviderConfig config;
        config.domain_id = args.domain_id;
        config.document = args.document;
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
