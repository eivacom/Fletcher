# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class FletcherArrowBridgeBenchmarksConan(ConanFile):
    """Benchmark consumer for the Arrow-tier serialization path.

    Not published as a Conan package, and not part of arrow-bridge's or
    pubsub-arrow's exports_sources: this conanfile resolves the deps, writes
    the CMake toolchain, and drives the build via `conan build .`. The
    packages themselves are expected to be in the local cache already.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("fletcher-arrow-bridge/[*, include_prerelease]")
        self.requires("fletcher-pubsub-arrow/[*, include_prerelease]")
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        self.requires("benchmark/1.9.4")
        # Resolve a transitive zlib conflict between arrow (pins 1.2.13) and
        # openssl (range >=1.2.11 <2, resolves to 1.3.1). Pin to 1.3.1. Same
        # conflict arrow-bridge/conanfile.py:36 resolves.
        self.requires("zlib/1.3.1", override=True)

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
