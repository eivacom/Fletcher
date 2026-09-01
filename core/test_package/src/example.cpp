// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <cassert>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <vector>

int main() {
    // Envelope round-trip
    fletcher::Envelope env;
    env.row = {0x01, 0x02, 0x03, 0x04};

    auto serialized = fletcher::SerializeEnvelope(env);
    auto owner = std::make_shared<const std::vector<uint8_t>>(std::move(serialized));
    auto restored = fletcher::DeserializeEnvelope(owner);
    assert(restored.row == env.row);

    // PositionalWriter over an owning buffer: write one bool field, then Finish() for the bytes
    fletcher::VectorWriteBuffer writeBuffer;
    fletcher::PositionalWriter positionalWriter(writeBuffer, 1 /*num_fields*/);
    positionalWriter.WriteBool(false);
    std::vector<uint8_t> raw = writeBuffer.Finish();

    // Blob: an owner plus a span. Two ways in — bytes Fletcher allocated...
    fletcher::Blob owned_blob{std::move(raw)};
    assert(!owned_blob.empty());

    // ...and bytes it did not, handed over where they lie with an owner that
    // keeps them alive.
    auto arena = std::make_shared<const std::vector<uint8_t>>(4, 0x7F);
    fletcher::Blob borrowed_blob(arena, arena->data(), arena->size());
    assert(borrowed_blob.data() == arena->data());
}
