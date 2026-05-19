# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherPubsubConan(ConanFile):
    name = "eiva-fletcher-xrcedds-pubsub-provider"
    version = "0.1.0-alpha"
    description = "XRCE-DDS PubSub Provider library"
    license = "LGPL-3.0-or-later"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {"run_tests": False}

    exports_sources = (
        "CMakeLists.txt",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
        "third_party/*",
    )

    def requirements(self):
        self.requires("eiva-fletcher-pubsub/0.1.1-alpha", transitive_headers=True)
        self.requires("eiva-fletcher-core/0.1.5-alpha", transitive_headers=True)
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
            self.run(
                f'ctest --test-dir "{self.build_folder}" '
                f'-C {self.settings.build_type} --output-on-failure'
            )

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
        # The xrce_dds_pubsub_provider static library links privately
        # against microxrcedds_client (which in turn depends on microcdr),
        # both built from source via FetchContent and packaged alongside.
        # External consumers must link all three in dependency order so
        # the uxr_*/ucdr_* symbols resolve.
        self.cpp_info.libs = [
            "xrce_dds_pubsub_provider",
            "microxrcedds_client",
            "microcdr",
        ]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "eiva-fletcher-xrcedds-pubsub-provider")
        self.cpp_info.set_property("cmake_target_name", "eiva-fletcher-xrcedds-pubsub-provider::eiva-fletcher-xrcedds-pubsub-provider")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-xrcedds-pubsub-provider-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
