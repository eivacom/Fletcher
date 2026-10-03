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

#include <string>

namespace fletcher::csharp_backend {

class CsVisitor {
   public:
    explicit CsVisitor(const google::protobuf::FileDescriptor* file);

    // The whole <stem>.fletcher.cs for the file.
    std::string GenerateFile();

   private:
    std::string GenerateEnum(const google::protobuf::EnumDescriptor* enm);
    std::string GenerateMessage(const google::protobuf::Descriptor* msg);

    const google::protobuf::FileDescriptor* file_;
};

}  // namespace fletcher::csharp_backend
