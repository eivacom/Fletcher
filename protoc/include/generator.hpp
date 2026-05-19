// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/compiler/code_generator.h>
#include <cstdint>
#include <string>

namespace fletcher {

class ArrowRowGenerator : public google::protobuf::compiler::CodeGenerator {
 public:
    bool Generate(const google::protobuf::FileDescriptor* file,
                  const std::string& parameter,
                  google::protobuf::compiler::GeneratorContext* context,
                  std::string* error) const override;

    // Advertise proto3-optional support so protoc passes files with
    // `optional` fields in proto3 syntax through to this plugin.
    uint64_t GetSupportedFeatures() const override {
        return FEATURE_PROTO3_OPTIONAL;
    }
};

}  // namespace fletcher
