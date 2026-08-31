// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <cassert>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <vector>

int main() {
    // Envelope round-trip
    fletcher::Envelope env;
    env.row = {0x01, 0x02, 0x03, 0x04};

    auto serialized = fletcher::SerializeEnvelope(env);
    auto restored = fletcher::DeserializeEnvelope(serialized);
    assert(restored.row == env.row);

    // PositionalWriter over an owning buffer: write one bool field, then Finish() for the bytes
    fletcher::VectorWriteBuffer writeBuffer;
    fletcher::PositionalWriter positionalWriter(writeBuffer, 1 /*num_fields*/);
    positionalWriter.WriteBool(false);
    std::vector<uint8_t> raw = writeBuffer.Finish();

    // Blob: shared_ptr to a const byte vector
    fletcher::Blob blob = std::make_shared<const std::vector<uint8_t>>(std::move(raw));
    assert(blob && !blob->empty());
}
