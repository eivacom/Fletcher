# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class FletcherCAbiBenchmarksConan(ConanFile):
    """Benchmark consumer for BIND-4d-v's per-row publish benchmark (D-BIND-48).

    Not published as a Conan package, and not part of c-abi's exports_sources:
    this conanfile resolves the deps, writes the CMake toolchain, and drives the
    build via `conan build .`. The packages themselves are expected to be in the
    local cache already, exactly as for arrow-bridge/benchmarks.

    It links BOTH the generated C++ publisher (fletcher-pubsub, statically) and
    the binding shim (fletcher-c-abi, dynamically), so one executable measures
    the baseline and the shim side by side. Two copies of the pub/sub layer in
    one process is deliberate and safe: the shim exports only `fl_*` (its
    version script on Linux, dllexport on Windows), so neither copy can resolve
    to the other.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("fletcher-protoc/[*, include_prerelease]")
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-c-abi/[*, include_prerelease]")
        # Required directly because CMakeLists invokes `protobuf::protoc` to run
        # the plugin; the protoc package does not propagate it as a target. The
        # same reason integration-tests/protoc-arrow-bridge gives.
        self.requires("protobuf/3.21.12")
        self.requires("benchmark/1.9.4")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
