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

#include <optional>
#include <set>
#include <string>

#include "csharp_backend_columns.hpp"
#include "option_metadata.hpp"

namespace fletcher::csharp_backend {

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

    // The whole <stem>.fletcher.gateway.cs (BIND-8c, D-BIND-109): per pub/sub method, a
    // `<Svc>_<Method>GatewayHandler` delegate and the `<Svc>_<Method>GatewayPublisher` /
    // `<Svc>_<Method>GatewaySubscriber` pair over a borrowed Eiva.Fletcher.GatewayClient
    // FletcherClient (D-BIND-115).
    std::string GenerateGatewayFile();

   private:
    std::string GenerateEnum(const google::protobuf::EnumDescriptor* enm);
    std::string GenerateMessage(const google::protobuf::Descriptor* msg);

    const google::protobuf::FileDescriptor* file_;
    CrossFile cross_file_;
    const OptionMetadataResolver* resolver_ = nullptr;
};

// A refusal when two types the generated C# of `file` declares would share a name (D-BIND-100):
// its messages and enums, a topic class per pub/sub method, and the native and gateway pair
// types when those are generated. nullopt when every name is distinct.
std::optional<std::string> CsPairNameConflict(const google::protobuf::FileDescriptor* file,
                                              bool native_pair, bool gateway_pair);

}  // namespace fletcher::csharp_backend
