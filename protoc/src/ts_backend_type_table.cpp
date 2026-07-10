// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ts_backend_type_table.hpp"

#include <string>

namespace fletcher::ts_backend {

// ---------------------------------------------------------------------------
// Scalar lookup: language-neutral logical identity -> TypeScript backend
// strings. These are the ONLY TS type / WireTypeId strings in the pipeline
// (locked #1). The field order matches TsScalarInfo: {ts_type_text, wire_type_id}.
// The mapping reproduces the former type_mapper::TsScalarType +
// generator.cpp::TsWireTypeId / WireTypeIdName behavior exactly.
// ---------------------------------------------------------------------------

const TsScalarInfo& TsLookupScalar(const ir::LogicalType& type,
                                   const std::optional<ir::EnumIdentity>& enum_identity) {
    static const TsScalarInfo kBool{"boolean", "WireTypeId.BOOL"};
    static const TsScalarInfo kInt32{"number", "WireTypeId.INT32"};
    static const TsScalarInfo kInt64{"bigint", "WireTypeId.INT64"};
    static const TsScalarInfo kUInt32{"number", "WireTypeId.UINT32"};
    static const TsScalarInfo kUInt64{"bigint", "WireTypeId.UINT64"};
    static const TsScalarInfo kFloat{"number", "WireTypeId.FLOAT32"};
    static const TsScalarInfo kDouble{"number", "WireTypeId.FLOAT64"};
    static const TsScalarInfo kString{"string", "WireTypeId.STRING"};
    static const TsScalarInfo kBytes{"Uint8Array", "WireTypeId.BINARY"};
    static const TsScalarInfo kTimestamp{"bigint", "WireTypeId.TIMESTAMP_NANO"};
    static const TsScalarInfo kDuration{"bigint", "WireTypeId.DURATION_NANO"};
    static const TsScalarInfo kUnknown{"unknown", "WireTypeId.UNKNOWN"};

    // Enum lowers to INT32 storage (enum_identity present) and renders identically
    // to a plain int32 in GIR-7 — same TS text and wire type either way. The
    // parameter is retained so a future backend / GIR-9 can key on it.
    (void)enum_identity;

    using LK = ir::LogicalKind;
    switch (type.kind) {
        case LK::BOOL:
            return kBool;
        case LK::INT32:
            return kInt32;
        case LK::INT64:
            return kInt64;
        case LK::UINT32:
            return kUInt32;
        case LK::UINT64:
            return kUInt64;
        case LK::FLOAT32:
            return kFloat;
        case LK::FLOAT64:
            return kDouble;
        case LK::UTF8:
            return kString;
        case LK::BINARY:
        case LK::FIXED_SIZE_BINARY:
            return kBytes;
        case LK::TIMESTAMP:
        case LK::WKT_TIMESTAMP:
            return kTimestamp;
        case LK::DURATION:
        case LK::WKT_DURATION:
            return kDuration;
        default:
            return kUnknown;
    }
}

// ---------------------------------------------------------------------------
// Structural naming (TypeScript backend concern). Prepend "I" for interfaces;
// concatenate containing message names with "_" for nested types.
// ---------------------------------------------------------------------------

std::string TsSchemaConstName(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return name;
}

std::string TsInterfaceName(const google::protobuf::Descriptor* msg) {
    return "I" + TsSchemaConstName(msg);
}

}  // namespace fletcher::ts_backend
