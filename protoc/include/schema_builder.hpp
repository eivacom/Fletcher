// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <nanoarrow/nanoarrow.hpp>
#include <vector>

#include "option_metadata.hpp"

namespace fletcher {

// Build the Arrow schema for msg in-process. This is the in-process execution
// sink of the ONE IR-driven schema visitor (GIR-5): the same
// cpp_backend::SchemaVisitor that emits the generated <Class>Schema() source
// also drives this build (via a nanoarrow sink), so the two are byte-identical
// by construction — same field order, names, nullability flags, and metadata.
// Defined in generator.cpp (thin wrapper over the visitor). Throws
// std::runtime_error on failure.
//
// GIR-13: `resolver` (nullable) supplies --fletcher_opt=metadata_from_option
// extras. nullptr == "no rules given" and reproduces the pre-GIR-13 bytes exactly.
nanoarrow::UniqueSchema BuildMessageSchema(const google::protobuf::Descriptor* msg,
                                           const OptionMetadataResolver* resolver = nullptr);

// Serialize an ArrowSchema to Arrow IPC stream format (schema message +
// end-of-stream marker, no batches). Byte-compatible with
// fletcher::SerializeSchemaIpc in fletcher-pubsub, which providers use to
// announce schemas at runtime. Throws std::runtime_error on failure.
std::vector<uint8_t> SerializeSchemaIpc(const ArrowSchema* schema);

}  // namespace fletcher
