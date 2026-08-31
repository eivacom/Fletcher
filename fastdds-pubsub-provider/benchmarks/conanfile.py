# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class FletcherPubSubTypeBenchmarksConan(ConanFile):
    """Benchmark consumer for the provider's DDS types.

    Not published as a Conan package, and not part of the provider's
    exports_sources: this conanfile resolves the deps, writes the CMake
    toolchain, and drives the build via `conan build .`. The provider
    itself is expected to be in the local cache already.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        self.requires("benchmark/1.6.1")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
