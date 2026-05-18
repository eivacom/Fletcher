# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherProtocPluginConan(ConanFile):
    name = "eiva-fletcher-protoc"
    version = "0.2.0-alpha"
    description = "A protoc plugin that generates C++ and TypeScript code for the Fletcher wire format"
    license = "Proprietary"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {"run_tests": False}

    exports_sources = (
        "CMakeLists.txt",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
    )

    def requirements(self):
        self.requires("protobuf/3.21.12", visible=True)
        if self.options.run_tests:
            self.requires("gtest/1.17.0")

    def package_id(self):
        del self.info.options.run_tests

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        if self.options.run_tests:
            tc.cache_variables["FLETCHER_BUILD_TESTS"] = "ON"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if self.options.run_tests:
            cmake.test()

    def package(self):
        # Package the executable.
        copy(self, "fletcher-protoc*",
             src=os.path.join(self.build_folder, str(self.settings.build_type)),
             dst=os.path.join(self.package_folder, "bin"),
             keep_path=False)
        # Single-config generators (Make/Ninja on Linux) place the binary directly
        # in the build folder without a Release/Debug subdirectory.
        copy(self, "fletcher-protoc",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "bin"),
             keep_path=False)
        # Package the CMake target module.
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)

    def package_info(self):
        # No libraries to link — this package only provides the plugin executable
        # and a CMake module that creates the imported target.
        self.cpp_info.libdirs = []
        self.cpp_info.includedirs = []
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-protoc-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
