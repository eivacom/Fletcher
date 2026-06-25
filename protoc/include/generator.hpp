// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <string>
#include <vector>

namespace fletcher {

class ArrowRowGenerator : public google::protobuf::compiler::CodeGenerator {
   public:
    bool Generate(const google::protobuf::FileDescriptor* file, const std::string& parameter,
                  google::protobuf::compiler::GeneratorContext* context,
                  std::string* error) const override;

    // Override the all-files hook so the shared Rust `__rba.fletcher.rs` helper
    // module is emitted EXACTLY ONCE per protoc invocation, regardless of how many
    // .proto files are passed in one invocation (code-review P1). Per-file
    // artifacts are still produced by Generate() (called once per file here).
    bool GenerateAll(const std::vector<const google::protobuf::FileDescriptor*>& files,
                     const std::string& parameter,
                     google::protobuf::compiler::GeneratorContext* context,
                     std::string* error) const override;

    // Advertise proto3-optional support so protoc passes files with
    // `optional` fields in proto3 syntax through to this plugin.
    uint64_t GetSupportedFeatures() const override { return FEATURE_PROTO3_OPTIONAL; }
};

}  // namespace fletcher
