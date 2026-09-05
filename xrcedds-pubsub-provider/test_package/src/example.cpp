// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <cstdio>
#include <cstring>
#include <fletcher/core/envelope.hpp>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <memory>
#include <string>
#include <vector>

using namespace fletcher;

int main() {
    // PDA-DEC-7. This much is compiled and run against the INSTALLED header, and it is a
    // machine check on two claims rather than a demonstration:
    //
    //  1. the header advises `kPayloadBytes<N>`, so it must declare it - writing the idiom here
    //     is what stops `payload_bound.hpp` being dropped from that header in silence (the
    //     Fast DDS provider had exactly that regression once);
    //  2. the whole configuration surface is `ProviderConfig` plus one registration call. No
    //     XRCE type is nameable from out of tree, so a resurrected options struct would have to
    //     appear here to be used at all.
    //
    // Registration touches no socket and needs no Agent, which is why it is safe in a
    // test_package. Constructing a provider is not, and is not attempted.
    ProviderRegistry registry;
    RegisterXrceProvider(registry);

    ProviderConfig config;
    config.domain_id = 0;
    config.max_payload_bytes = kPayloadBytes<64 * 1024>;
    config.document = "transport=udp\nagent=127.0.0.1:2018";

    if (config.max_payload_bytes != 65536u) {
        std::fputs("FAIL: kPayloadBytes<64 * 1024> is not 65536\n", stderr);
        return 1;
    }

    Envelope env;
    env.row = {0x01, 0x02, 0x03};

    const std::vector<uint8_t> payload{0xDE, 0xAD};
    env.attachments["sensor"] = Blob{payload};

    // Parsing needs an owner for the bytes: the attachments alias them.
    auto wire = std::make_shared<const std::vector<uint8_t>>(SerializeEnvelope(env));
    auto restored = DeserializeEnvelope(wire);

    if (restored.row != env.row) {
        std::fputs("FAIL: row data mismatch after round-trip\n", stderr);
        return 1;
    }

    if (restored.attachments.size() != 1) {
        std::fprintf(stderr, "FAIL: expected 1 attachment, got %zu\n", restored.attachments.size());
        return 1;
    }

    const Blob& sensor = restored.attachments.at("sensor");
    if (sensor.size() != payload.size() ||
        std::memcmp(sensor.data(), payload.data(), payload.size()) != 0) {
        std::fputs("FAIL: attachment data mismatch after round-trip\n", stderr);
        return 1;
    }

    std::fputs("PASS: envelope round-trip OK\n", stdout);
    return 0;
}
