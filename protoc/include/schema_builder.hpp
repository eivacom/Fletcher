// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <nanoarrow/nanoarrow.hpp>
#include <vector>

namespace fletcher {

// Build the Arrow schema for msg in-process. Mirrors the <Class>Schema()
// function the generator emits into .fletcher.pb.h — same nanoarrow calls,
// same field order, names, nullability flags, and metadata — so the result
// is identical to the schema the generated code constructs at runtime.
// Defined in generator.cpp because it shares the field-gathering internals
// with code generation. Throws std::runtime_error on failure.
nanoarrow::UniqueSchema BuildMessageSchema(const google::protobuf::Descriptor* msg);

// Serialize an ArrowSchema to Arrow IPC stream format (schema message +
// end-of-stream marker, no batches). Byte-compatible with
// fletcher::SerializeSchemaIpc in fletcher-pubsub, which providers use to
// announce schemas at runtime. Throws std::runtime_error on failure.
std::vector<uint8_t> SerializeSchemaIpc(const ArrowSchema* schema);

}  // namespace fletcher
