# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherPubsubConan(ConanFile):
    name = "fletcher-fastdds-pubsub-provider"
    version = "0.3.2-alpha"
    description = "Fletcher FastDDS PubSub Provider library"
    license = "LGPL-3.0-or-later"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {
        "run_tests": False,
        # Link the Fast DDS chain statically by default. This recipe owns the
        # fast-dds requirement, so the static default lives here rather than in
        # a blanket *:shared profile option — consumers (notably the gateway)
        # then bake Fast DDS into a self-contained binary that runs with no
        # conanrun / LD_LIBRARY_PATH. Opt back into shared by layering the
        # `shared` profile fragment (a higher-priority profile option wins).
        "fast-dds/*:shared": False,
        "fast-cdr/*:shared": False,
        "foonathan-memory/*:shared": False,
        "tinyxml2/*:shared": False,
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
        self.requires("fletcher-pubsub/0.3.1-alpha", transitive_headers=True)
        self.requires("fletcher-core/0.3.1-alpha", transitive_headers=True)
        # FastDDS headers are part of this package's public API
        # (FastDDSProviderOptions exposes DataWriterQos / DataReaderQos),
        # so downstream consumers must see them transitively.
        self.requires("eiva-fast-dds/3.3.0a1", transitive_headers=True)
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
        # Public pubsub headers.
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        # Built static library.
        copy(self, "*.a",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.lib",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        # CMake target alias module.
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["fletcher-fastdds-pubsub-provider"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "fletcher-fastdds-pubsub-provider")
        self.cpp_info.set_property("cmake_target_name", "fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-fastdds-pubsub-provider-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
