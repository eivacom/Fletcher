# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherCopyProbeConan(ConanFile):
    """The copy-accounting oracle's instrument: TEST-ONLY, never released.

    The ledger, the one `Judge()`, `SeamProbeProvider` and the source-tracing
    window (D-BIND-61). It lives in a package of its own because two builds
    score with it and neither can reach the other's tree (D-BIND-62): the
    pubsub-conformance harness, and c-abi's `fletcher-c-abi-probe`, which c-abi
    builds only under its `with_probe_shim` option - and CI builds c-abi with
    `conan create`, which sees nothing outside `c-abi/`.

    Released Fletcher packages never require this one: c-abi requires it only
    under `with_probe_shim`, and release builds leave that off.
    """

    name = "fletcher-copy-probe"
    version = "0.5.0-alpha"
    description = "Test-only copy-accounting instrument shared by Fletcher's test harnesses"
    license = "LGPL-3.0-or-later"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    # fPIC for the reason fletcher-pubsub states: c-abi links this archive into a
    # shared library (the probe shim) on Linux.
    options = {"run_tests": [True, False], "fPIC": [True, False]}
    default_options = {"run_tests": False, "fPIC": True}

    exports_sources = (
        "CMakeLists.txt",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
    )

    def requirements(self):
        # transitive_headers: the public headers name PubSubProvider and
        # WriteBuffer, so a consumer compiles against pubsub's headers too.
        self.requires("fletcher-pubsub/0.5.1-alpha", transitive_headers=True)
        if self.options.run_tests:
            self.requires("gtest/1.17.0")

    def package_id(self):
        del self.info.options.run_tests

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

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
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        copy(self, "*fletcher-copy-probe.a",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*fletcher-copy-probe.lib",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["fletcher-copy-probe"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "fletcher-copy-probe")
        self.cpp_info.set_property("cmake_target_name",
                                   "fletcher-copy-probe::fletcher-copy-probe")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-copy-probe-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
