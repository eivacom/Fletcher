# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherProtocPluginConan(ConanFile):
    name = "fletcher-protoc"
    version = "0.3.2-alpha"
    description = "A protoc plugin that generates C++ and TypeScript code for the Fletcher wire format"
    license = "LGPL-3.0-or-later"
    package_type = "application"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {
        "run_tests": False,
        # Link protobuf statically so the plugin binary is self-contained.
        # protoc spawns fletcher-protoc(.exe) as a subprocess with no Conan
        # environment active, so a shared protobuf would not be found at runtime.
        # Pinned in the recipe (not a workflow -o flag) so the intent travels
        # with the package; static is also the default now that the profiles no
        # longer force *:shared=True.
        "protobuf/*:shared": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
        "third_party/*",
    )

    def requirements(self):
        self.requires("protobuf/3.21.12", visible=True)
        if self.options.run_tests:
            # test_requires (rather than requires) so gtest does NOT show up in
            # self.dependencies.host during package() — otherwise its DLLs would
            # be copied next to fletcher-protoc.exe on Windows, leaking test-only
            # binaries into the shipped package. package_id() ignores run_tests,
            # so a run_tests=True build must produce the same package contents
            # as a run_tests=False build.
            self.test_requires("gtest/1.17.0")

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
        # Bundle dependency DLLs next to the exe on Windows. protoc spawns
        # fletcher-protoc.exe as a plugin subprocess with no Conan environment
        # active, so DLLs must be co-located (first entry in Windows DLL search).
        if self.settings.os == "Windows":
            for dep in self.dependencies.host.values():
                for bindir in dep.cpp_info.bindirs:
                    copy(self, "*.dll", src=bindir,
                         dst=os.path.join(self.package_folder, "bin"),
                         keep_path=False)
        # Package the CMake target module.
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)
        # Package Fletcher's public proto option definitions for consumers.
        copy(self, "*.proto",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)

    def package_info(self):
        # No libraries to link — this package only provides the plugin executable
        # and a CMake module that creates the imported target.
        self.cpp_info.libdirs = []
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-protoc-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
