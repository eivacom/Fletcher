// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// JSON ↔ nanoarrow ArrowSchema conversion. The gateway uses these to
// translate between the SchemaDescriptor JSON that travels over the
// WebSocket protocol and the OwnedSchema objects that `pubsub`'s
// Publisher consumes.
//
// Internal to the gateway exe — not part of any public include path
// or installed interface. Lives in its own translation unit so unit
// tests can exercise both directions without standing up a
// Boost.Beast WebSocket.

#ifndef FLETCHER_GATEWAY_SCHEMA_CODEC_HPP_
#define FLETCHER_GATEWAY_SCHEMA_CODEC_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <fletcher/pubsub/owned_schema.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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

/// The schema a `create_topic` message declares (D-BIND-112):
///   * `schemaIpc` (a string): base64 of the topic's Arrow IPC schema, decoded
///     with `DeserializeSchemaIpc`. Full fidelity, so a gateway client's
///     declaration is byte-identical to a native publisher's of the same
///     generated schema, and re-declaring the topic is idempotent;
///   * else `schema`: the JSON descriptor, through `BuildArrowSchemaFromJson`
///     (scalar fields only, nullability and metadata dropped), kept for
///     existing clients;
///   * else no schema (an empty `OwnedSchema`): the topic routes bytes only.
/// `schemaIpc` wins when both are present. Throws `std::invalid_argument`
/// when `schemaIpc` is not a string, not base64, or not an Arrow IPC schema.
OwnedSchema DeclaredSchemaFromCreateTopic(const nlohmann::json& msg);

/// Standard base64 (RFC 4648, `+/`, `=` padding). The `subscribed` reply's
/// `schemaIpc` is written with it.
std::string Base64Encode(const uint8_t* data, size_t len);

/// The inverse of `Base64Encode`, strict: the length must be a multiple of 4,
/// padding only at the end, and every other character from the alphabet.
/// Throws `std::invalid_argument` otherwise.
std::vector<uint8_t> Base64Decode(const std::string& text);

}  // namespace fletcher::gateway

#endif  // FLETCHER_GATEWAY_SCHEMA_CODEC_HPP_
