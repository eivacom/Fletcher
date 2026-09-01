// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <cstdio>
#include <cstring>
#include <fletcher/core/envelope.hpp>
#include <memory>
#include <vector>

using namespace fletcher;

int main() {
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
