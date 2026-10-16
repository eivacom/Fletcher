# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class XrceAgentForTransportConformanceConan(ConanFile):
    """Builds the MicroXRCEAgent the C# cross-transport suite needs (D-BIND-56).

    Not a package: `conan build .` resolves Fast DDS, writes the toolchain, and
    builds the Agent through integration-tests/cmake/MicroXrceAgent.cmake.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    # The Agent links Conan's Fast DDS rather than cloning its own (see the
    # shared recipe for why that matters on MSVC). Static, as the Fast DDS
    # provider builds it, so this resolves the binary the lane already built.
    default_options = {"fast-dds/*:shared": False}

    def requirements(self):
        # Directly, so Fast DDS and Fast CDR reach CMakeDeps WITH headers - the
        # Agent compiles against them. The same reason as in
        # integration-tests/fastdds-xrce-interop/conanfile.py.
        self.requires("fast-dds/3.4.0")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
