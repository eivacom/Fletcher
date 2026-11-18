# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Builds the production gateway exe for the C# gateway client's live lane (BIND-8b). The C#
# test project spawns it; it is found under build/ or named by GATEWAY_BIN.
cmake_minimum_required(VERSION 3.15)
project(gateway-dotnet-end-to-end-integration CXX)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway"
    "${CMAKE_CURRENT_BINARY_DIR}/gateway_build")
