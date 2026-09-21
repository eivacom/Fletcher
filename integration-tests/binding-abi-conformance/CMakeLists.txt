# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(binding-abi-conformance CXX)

find_package(fletcher-c-abi CONFIG REQUIRED)
find_package(arrow CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(emit_corpus src/emit_corpus.cpp)

# The corpus header lives with the codec's own tests and is included from there
# rather than copied here. That is the whole point of this suite: the scenarios
# C# round-trips are the SAME objects `NanoarrowCodec.ByteIdenticalToArrowBridge`
# compares the two C++ encoders over (D-BIND-35, D-BIND-40). A copy would drift,
# and both copies would keep passing while covering different things.
target_include_directories(emit_corpus PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../c-abi/tests")

# Arrow C++ builds the fixtures and writes the IPC the C# side rebuilds from; the
# shim supplies the wire bytes. The emitter needs both, and it is deliberate that
# the SHIM does not link Arrow C++ — only this emitter does, exactly as the
# codec's own test does.
target_link_libraries(emit_corpus PRIVATE fletcher-c-abi::fletcher-c-abi arrow::arrow)
