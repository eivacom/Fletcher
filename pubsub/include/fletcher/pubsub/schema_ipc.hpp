// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_SCHEMA_IPC_HPP_
#define FLETCHER_INCLUDE_PUBSUB_SCHEMA_IPC_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <vector>

#include "fletcher/pubsub/owned_schema.hpp"

namespace fletcher {

/// Serialize an ArrowSchema to IPC stream format (schema-only, no batches).
/// The result is a self-contained byte buffer suitable for storage or
/// transmission.  Wire-compatible with Apache Arrow C++ IPC.
std::vector<uint8_t> SerializeSchemaIpc(const ArrowSchema* schema);

/// Deserialize an ArrowSchema from IPC stream bytes produced by
/// SerializeSchemaIpc.
OwnedSchema DeserializeSchemaIpc(const uint8_t* data, size_t len);

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_SCHEMA_IPC_HPP_
