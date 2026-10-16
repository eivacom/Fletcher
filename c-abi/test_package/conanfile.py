# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
import os

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout
from conan.tools.build import can_run


class CAbiTestConan(ConanFile):
    """Consume the packaged shim the way a language binding does.

    The example is a C program (project languages: C only) that includes one
    header and links the shipped shared library. If the package ever starts
    requiring a C++ header, an Arrow header or an eProsima header to be usable,
    this build stops compiling — which is the point.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires(self.tested_reference_str)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def layout(self):
        cmake_layout(self)

    def test(self):
        if can_run(self):
            cmd = os.path.join(self.cpp.build.bindir, "example")
            # env="conanrun" puts the shim's directory on PATH / LD_LIBRARY_PATH:
            # unlike every other Fletcher component this one is a SHARED library,
            # so the loader has to find it at run time.
            self.run(cmd, env="conanrun")
