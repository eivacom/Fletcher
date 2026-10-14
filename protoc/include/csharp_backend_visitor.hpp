// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// BIND-6a: the C# backend visitor. It walks the language-neutral IR, as TsVisitor
// does, and takes every C# string from csharp_backend_type_table: no C# text on an
// IR node (GIR locked decision #1). The message walk reuses
// cpp_backend::BuildFlattenedFieldList, so the C# field set cannot drift from the
// schema's.
//
// Generated C# writes no wire bytes (D-BIND-1): the row class converts to and from
// Arrow, and the one C++ codec behind the C ABI does the encoding.

#include <google/protobuf/descriptor.h>

#include <set>
#include <string>

#include "option_metadata.hpp"

namespace fletcher::csharp_backend {

// The messages from other .proto files that the file being generated names, from the
// one walk the C++ backend derives its includes from (CollectCrossFileMessages,
// D-BIND-87). A struct column of such a message is built through its class's public
// ToArrow (D-BIND-80).
using CrossFile = std::set<const google::protobuf::Descriptor*>;

class CsVisitor {
   public:
    // `resolver` (nullable) is the generator's metadata_from_option resolver, so the
    // C# `Schema` carries the same extra metadata as the C++ one (BIND-6b).
    explicit CsVisitor(const google::protobuf::FileDescriptor* file,
                       const OptionMetadataResolver* resolver = nullptr);

    // The whole <stem>.fletcher.cs for the file: the model layer, Apache.Arrow only,
    // including a `<Svc>_<Method>Topic` class per pub/sub method (D-BIND-77).
    std::string GenerateFile();

    // The whole <stem>.fletcher.native.cs (BIND-6d, D-BIND-76): per pub/sub method, a
    // `<Svc>_<Method>Handler` delegate and the `<Svc>_<Method>Publisher` /
    // `<Svc>_<Method>Subscriber` pair over Eiva.Fletcher's native runtime.
    std::string GenerateNativeFile();

   private:
    std::string GenerateEnum(const google::protobuf::EnumDescriptor* enm);
    std::string GenerateMessage(const google::protobuf::Descriptor* msg);

    const google::protobuf::FileDescriptor* file_;
    CrossFile cross_file_;
    const OptionMetadataResolver* resolver_ = nullptr;
};

}  // namespace fletcher::csharp_backend
