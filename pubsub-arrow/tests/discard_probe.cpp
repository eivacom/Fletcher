// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// HARD-6 forcing translation unit (negative-compile).
//
// Every statement below intentionally DISCARDS the return value of a
// [[nodiscard]]-annotated HARD-6 public API. This TU is EXCLUDE_FROM_ALL and is
// compiled only by the `NodiscardTest.CompileFailsOnDiscard` CTest entry with
// C4834 promoted to an error (MSVC `/we4834`) or `-Werror=unused-result`
// (gcc/clang). With the annotations present the discards emit the nodiscard
// diagnostic and the test PASSES; with the annotations removed the TU compiles
// clean, no diagnostic appears, and the test FAILS (red-first polarity).
//
// NOTE: this file (and its object-library target) deliberately avoid the
// substring "nodiscard" in their names. MSVC/cl echoes the compiled source
// filename and the MSBuild target name to stdout, which the CTest
// PASS_REGULAR_EXPRESSION scans; a "nodiscard" in the name would self-match and
// mask the red state. Only a genuine diagnostic must supply "nodiscard".
//
// This file must never be added to a normal build target.

#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <vector>

void DiscardCore(fletcher::PositionalReader& reader, const fletcher::Envelope& envelope,
                 const std::vector<uint8_t>& bytes) {
    reader.IsNull(0);
    fletcher::SerializeEnvelope(envelope);
    fletcher::DeserializeEnvelope(bytes);
    fletcher::DeserializeEnvelope(bytes.data(), bytes.size());
}

void DiscardPubSub(fletcher::Publisher& publisher, fletcher::Subscriber& subscriber,
                   fletcher::PubSubProvider& provider, const ArrowSchema* schema) {
    publisher.ListTopics();
    subscriber.Subscribe({"probe"}, {});
    provider.Subscribe({"probe"}, {});
    fletcher::OwnedSchema::DeepCopy(schema);
}

void DiscardCodec(fletcher::Codec& codec, const fletcher::ArrowRow& row,
                  const fletcher::EncodedRow& bytes) {
    codec.EncodeRow(row);
    codec.DecodeRow(bytes);
    codec.DecodeRow(bytes.data(), bytes.size());
}
