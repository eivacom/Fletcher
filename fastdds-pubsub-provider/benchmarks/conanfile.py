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
        # An EXPLICIT fast-dds require, because PDA-DEC-6 dropped `transitive_headers=True` from
        # the provider recipe: the provider's public header names no eProsima type any more, so a
        # consumer no longer sees Fast DDS headers transitively. All four benchmark TUs include
        # <fastdds/...> directly (they exercise the DDS types themselves, which is the point), so
        # they have to ask for the SDK themselves. Benchmarks are outside CI, so this would
        # otherwise rot silently rather than go red.
        self.requires("fast-dds/3.4.0")
        self.requires("benchmark/1.6.1")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
