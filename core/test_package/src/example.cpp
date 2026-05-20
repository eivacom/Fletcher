// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/core/envelope.hpp"
#include "fletcher/core/positional_io.hpp"
#include "fletcher/core/types.hpp"
#include "fletcher/core/write_buffer.hpp"

#include <cassert>
#include <vector>

int main() {
    // Envelope round-trip
    fletcher::Envelope env;
    env.row = { 0x01, 0x02, 0x03, 0x04 };

    auto serialized = fletcher::SerializeEnvelope(env);
    auto restored   = fletcher::DeserializeEnvelope(serialized);
    assert(restored.row == env.row);

    // PositionalWriter: backing vector + buffer wrapper, then write one bool field
    std::vector<uint8_t> raw;
    fletcher::VectorWriteBuffer writeBuffer(raw);
    fletcher::PositionalWriter positionalWriter(writeBuffer, 1 /*num_fields*/);
    positionalWriter.WriteBool(false);

    // Blob: shared_ptr to a const byte vector
    fletcher::Blob blob = std::make_shared<const std::vector<uint8_t>>(raw);
    assert(blob && !blob->empty());
}
