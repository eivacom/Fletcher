// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-7: the TypeScript backend lookup table. This is the ONLY place a
// TypeScript type string or a runtime WireTypeId member name is allowed to live
// (locked decision #1). It is keyed by the language-neutral IR logical identity
// (ir::LogicalType + optional ir::EnumIdentity) and turns that identity into the
// generated TypeScript strings ("boolean", "number", "bigint", "Uint8Array",
// "WireTypeId.INT32", ...). The IR nodes themselves carry no such strings. It is
// the first non-C++ backend table in the GIR rewrite and the proof-point that a
// second language backend consumes the same IR through its own table.

#include <google/protobuf/descriptor.h>

#include <optional>
#include <string>

#include "ir.hpp"

namespace fletcher::ts_backend {

// All TypeScript generated-code strings for one scalar logical identity: the
// interface type text and the runtime descriptor's WireTypeId member name.
struct TsScalarInfo {
    std::string ts_type_text;  // e.g. "boolean", "number", "bigint", "Uint8Array"
    std::string wire_type_id;  // e.g. "WireTypeId.BOOL", "WireTypeId.INT32"
};

// Map a language-neutral scalar logical identity to its TypeScript backend
// strings. `enum_identity` is preserved for GIR-9 symbol emission but does not
// alter GIR-7 output: enums lower to INT32 storage and render as `number` /
// `WireTypeId.INT32`, identical to a plain int32.
const TsScalarInfo& TsLookupScalar(const ir::LogicalType& type,
                                   const std::optional<ir::EnumIdentity>& enum_identity);

// TypeScript interface name for a message: Outer.Inner -> "IOuter_Inner".
std::string TsInterfaceName(const google::protobuf::Descriptor* msg);

// TypeScript TypedSchema const name for a message: Outer.Inner -> "Outer_Inner"
// (no "I" prefix, no "Schema" suffix). Single source of truth for the runtime
// `<Const>.fields` reference used by nested struct descriptors.
std::string TsSchemaConstName(const google::protobuf::Descriptor* msg);

}  // namespace fletcher::ts_backend
