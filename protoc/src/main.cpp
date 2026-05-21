// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <google/protobuf/compiler/plugin.h>

#include "generator.hpp"

int main(int argc, char* argv[]) {
    fletcher::ArrowRowGenerator generator;
    return google::protobuf::compiler::PluginMain(argc, argv, &generator);
}
