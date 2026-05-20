// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// JSON ↔ nanoarrow ArrowSchema conversion. The gateway uses these to
// translate between the SchemaDescriptor JSON that travels over the
// WebSocket protocol and the OwnedSchema objects that `pubsub`'s
// Driver consumes.
//
// Internal to the gateway exe — not part of any public include path
// or installed interface. Lives in its own translation unit so unit
// tests can exercise both directions without standing up a
// Boost.Beast WebSocket.

#ifndef FLETCHER_GATEWAY_SCHEMA_CODEC_HPP_
#define FLETCHER_GATEWAY_SCHEMA_CODEC_HPP_

#include <pubsub/owned_schema.hpp>

#include <nanoarrow/nanoarrow.h>

#include <nlohmann/json.hpp>

namespace fletcher::gateway {

/// Map a nanoarrow `ArrowType` to the Fletcher wire-type byte that
/// the TypeScript client recognises. Returns 0 for types the wire
/// format does not have an ID for.
int NanoarrowTypeToWireType(enum ArrowType type);

/// Inverse mapping for parsing a publisher-supplied schema in
/// `create_topic`. Scalar types only — anything that needs nested
/// nanoarrow setup (struct / list / map / etc.) throws an
/// `std::invalid_argument` with a clear message so the failure
/// surfaces in the gateway's WS error response instead of silently
/// producing a broken schema downstream.
enum ArrowType WireTypeToNanoarrowType(int wt);

/// Serialise a top-level ArrowSchema (struct of fields) to the JSON
/// SchemaDescriptor format the gateway-client-ts consumes. Walks
/// nested children for struct / list / map / fixed-size-list.
nlohmann::json ArrowSchemaToJson(const ArrowSchema* schema);

/// Parse the JSON SchemaDescriptor a publisher attached to
/// `create_topic` and build the corresponding nanoarrow struct
/// schema. Throws `std::invalid_argument` when:
///   * `j` does not contain a `fields` array;
///   * a field is missing `name` or `wireType`;
///   * a field's `wireType` is not a scalar covered by
///     WireTypeToNanoarrowType.
OwnedSchema BuildArrowSchemaFromJson(const nlohmann::json& j);

}  // namespace fletcher::gateway

#endif  // FLETCHER_GATEWAY_SCHEMA_CODEC_HPP_
