// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// What each column of a message converts as in C#, shared by the model class's
// ToArrow / FromArrow (BIND-6c) and the accessor and view (BIND-7), so the two read a
// column by one classification and cannot drift apart. Moved here from
// csharp_backend_visitor.cpp's anonymous namespace at BIND-7a, unchanged.

#include <google/protobuf/descriptor.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "csharp_backend_type_table.hpp"
#include "ir.hpp"

namespace fletcher::csharp_backend {

// The messages from other .proto files that the file being generated names, from the
// one walk the C++ backend derives its includes from (CollectCrossFileMessages,
// D-BIND-87). A struct column of such a message is built through its class's public
// ToArrow (D-BIND-80).
using CrossFile = std::set<const google::protobuf::Descriptor*>;

// One list element, one map key or one map value: a mapped scalar, a message whose own
// class converts, or (6c-3) itself a list of such elements, which is how a flatten
// wrapper around a repeated field nests: `repeated StructListWrapper` is a
// list<list<struct>>.
struct Elem {
    bool is_struct = false;
    bool is_list = false;
    CsScalarInfo scalar;                                // when neither
    std::string cls;                                    // when is_struct
    const google::protobuf::Descriptor* msg = nullptr;  // when is_struct: the message, for BIND-7
    bool other_file = false;      // when is_struct: the class is generated from another file
    std::shared_ptr<Elem> inner;  // when is_list: the element of this inner list
};

enum class ColKind { SCALAR, TEMPORAL, STRUCT, LIST, MAP };

struct Column {
    size_t index = 0;
    std::string property;
    ColKind kind = ColKind::SCALAR;
    bool nullable = false;
    CsScalarInfo scalar;                                // SCALAR
    bool is_timestamp = false;                          // TEMPORAL: Timestamp, else Duration
    std::string cls;                                    // STRUCT
    const google::protobuf::Descriptor* msg = nullptr;  // STRUCT: the message, for BIND-7
    bool other_file = false;  // STRUCT: the class is generated from another file
    Elem elem;                // LIST element, MAP value
    Elem key;                 // MAP key
};

// The kind a field without a C# type is reported as. Naming it in the output keeps
// the gap visible instead of dropping data silently.
const char* PendingKind(const ir::IrNode& node);

// The column for `node`, or nullopt with `why` set.
std::optional<Column> ClassifyColumn(const ir::IrNode& node,
                                     const google::protobuf::FileDescriptor* file,
                                     const CrossFile& cross,
                                     std::set<const google::protobuf::Descriptor*>& visiting,
                                     std::string& why);

// The first reason `msg` gets no ToArrow / FromArrow, or empty when every column
// converts. A message already being walked counts as converting: recursive messages
// are skipped whole by GenerateFile, so the cycle never reaches emission.
std::string ConversionBlocker(const google::protobuf::Descriptor* msg,
                              const google::protobuf::FileDescriptor* file, const CrossFile& cross,
                              std::set<const google::protobuf::Descriptor*>& visiting);

}  // namespace fletcher::csharp_backend
