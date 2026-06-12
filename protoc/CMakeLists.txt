# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
# C is needed for the vendored nanoarrow amalgamation.
project(fletcher-protoc LANGUAGES CXX C)

find_package(protobuf REQUIRED)

# Vendored nanoarrow (core + IPC) — used to build each message's Arrow
# schema in-process and serialize it to .ipc files (--fletcher_opt=ipc).
# Same amalgamation as pubsub/third_party/nanoarrow, so the serialized
# bytes match what fletcher::SerializeSchemaIpc produces at runtime.
add_subdirectory(third_party/nanoarrow)

# Core logic as a static library (reusable by tests).
add_library(fletcher_plugin_core STATIC
    src/type_mapper.cpp
    src/generator.cpp
    src/schema_ipc.cpp)

# C++ standard comes from the Conan profile (compiler.cppstd) — previously
# this CMakeLists pinned C++17, which overrode the profile's cppstd=20 and
# diverged from the protobuf it links. State the minimum at target scope.
target_compile_features(fletcher_plugin_core PUBLIC cxx_std_20)
target_include_directories(fletcher_plugin_core PUBLIC include PRIVATE src)
target_link_libraries(fletcher_plugin_core PUBLIC
    protobuf::libprotoc
    protobuf::libprotobuf
    nanoarrow)

# The protoc plugin executable.
add_executable(fletcher-protoc src/main.cpp)
target_link_libraries(fletcher-protoc PRIVATE fletcher_plugin_core)

# Tests (opt-in via FLETCHER_BUILD_TESTS, default OFF).
option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS fletcher-protoc DESTINATION bin)
