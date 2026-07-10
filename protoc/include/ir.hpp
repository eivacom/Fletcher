// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-3: the recursive, language-neutral mapped-type intermediate representation.
//
// This IR is the single canonical classification of a proto field/message into
// Fletcher's Arrow-shaped model. It carries ONLY language-neutral logical
// identity (LogicalKind/LogicalType, EnumIdentity, WKT markers, optionality) and
// descriptor references. It deliberately contains NO C++/Rust/TypeScript type
// string (locked decision #1): backend text (e.g. "arrow::int32()", "int32_t",
// "WriteInt32") lives only in the per-backend lookup tables
// (see cpp_backend_type_table.hpp). If you find yourself adding a
// single-language type/`std::`/header string on an IR node here, STOP.

#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fletcher::ir {

enum class NodeKind {
    SCALAR,
    LIST,
    FIXED_SIZE_LIST,
    STRUCT,
    MAP,
    UNSUPPORTED,
};

enum class LogicalKind {
    BOOL,

    INT8,
    INT16,
    INT32,
    INT64,
    UINT8,
    UINT16,
    UINT32,
    UINT64,

    FLOAT16,
    FLOAT32,
    FLOAT64,

    UTF8,
    BINARY,
    FIXED_SIZE_BINARY,

    DATE32,
    DATE64,
    TIMESTAMP,
    TIME32,
    TIME64,
    DURATION,

    DECIMAL,
    INTERVAL,

    WKT_TIMESTAMP,
    WKT_DURATION,

    // Wrapper WKT nodes use the wrapped scalar kind plus WktKind::WRAPPER_*.
    // Enum nodes use INT32 physical/logical storage plus EnumIdentity.
};

enum class TimeUnit {
    SECOND,
    MILLI,
    MICRO,
    NANO,
};

enum class IntervalUnit {
    YEAR_MONTH,
    DAY_TIME,
    MONTH_DAY_NANO,
};

enum class WktKind {
    NONE,
    WRAPPER_BOOL,
    WRAPPER_INT32,
    WRAPPER_INT64,
    WRAPPER_UINT32,
    WRAPPER_UINT64,
    WRAPPER_FLOAT,
    WRAPPER_DOUBLE,
    WRAPPER_STRING,
    WRAPPER_BYTES,
    TIMESTAMP,
    DURATION,
};

enum class DictionaryModifier {
    NONE,
    DICTIONARY,
};

struct LogicalType {
    LogicalKind kind;
    std::optional<int32_t> fixed_size_binary_width;
    std::optional<TimeUnit> time_unit;
    std::optional<std::string> timezone;
    std::optional<int32_t> decimal_precision;
    std::optional<int32_t> decimal_scale;
    std::optional<IntervalUnit> interval_unit;
};

struct EnumSymbol {
    std::string name;
    int32_t number;
};

struct EnumIdentity {
    const google::protobuf::EnumDescriptor* descriptor = nullptr;
    std::string full_name;
    std::vector<EnumSymbol> symbols;
};

// Language-neutral per-field facts. This is the SINGLE canonical home of
// `nullable` and `dictionary` (locked decision #3 / re-review fix 3a): node
// variants do NOT duplicate these. Both the encode visitor and the
// IR->FieldMapping projection read optionality/dictionary from here identically.
struct FieldFacts {
    const google::protobuf::FieldDescriptor* field_descriptor = nullptr;
    const google::protobuf::Descriptor* containing_message = nullptr;
    std::string proto_name;
    std::string proto_full_name;
    int32_t wire_field_id = 0;
    bool nullable = false;
    bool dictionary = false;
    bool repeated = false;
    bool map_entry = false;
    bool proto3_optional = false;
    bool proto2_optional = false;
    bool in_real_oneof = false;
    WktKind wkt = WktKind::NONE;
    std::vector<std::pair<std::string, std::string>> metadata;
    std::string warning;
};

struct StructIdentity {
    const google::protobuf::Descriptor* descriptor = nullptr;
    std::string full_name;
};

struct IrNode;

struct ScalarNode {
    LogicalType logical_type;
    std::optional<EnumIdentity> enum_identity;
};

struct ListNode {
    std::unique_ptr<IrNode> element;
};

struct FixedSizeListNode {
    std::unique_ptr<IrNode> element;
    int32_t size = 0;
};

// A field of a StructNode. Per re-review fix 3a there is NO `FieldFacts facts`
// here: the field's canonical facts live in `type->facts` (single source of
// truth). `name`/`field_number` are lightweight convenience mirrors of
// `type->facts.proto_name`/`type->facts.wire_field_id`, not an alternate facts
// home.
struct StructField {
    std::string name;
    int32_t field_number = 0;
    std::unique_ptr<IrNode> type;
};

struct StructNode {
    StructIdentity identity;
    std::vector<StructField> fields;
};

struct MapNode {
    std::unique_ptr<IrNode> key;
    std::unique_ptr<IrNode> value;
};

struct UnsupportedNode {
    std::string reason;
};

struct IrNode {
    NodeKind kind;
    FieldFacts facts;

    std::variant<ScalarNode, ListNode, FixedSizeListNode, StructNode, MapNode, UnsupportedNode>
        node;
};

// ---------------------------------------------------------------------------
// Construction (the single canonical classifier)
// ---------------------------------------------------------------------------

// Build the language-neutral IR node for a single proto field. This is the ONE
// classifier: MapField()/ProjectIrToFieldMapping() and the edge encode visitor
// both derive from BuildFieldIr() — there is no second independent classifier.
IrNode BuildFieldIr(const google::protobuf::FieldDescriptor* field);

// Build the struct IR for a whole message (identity + one child per field).
StructNode BuildMessageIr(const google::protobuf::Descriptor* message);

}  // namespace fletcher::ir
