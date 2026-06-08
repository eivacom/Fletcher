# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class GatewayBuildConan(ConanFile):
    """Build / test driver for the gateway exe.

    Gateway is distributed as a single executable — there is no Conan
    package for it (no installed headers, no consumable library). This
    conanfile exists only so CI and developers can run
    `conan install . && cmake --build && ctest` against the same
    dependency set the integration-test consumer uses.

    Never uploaded. Has no `name` / `version` so `conan create` is not
    a valid invocation; use `conan install` (optionally with
    `-o &:run_tests=True`) followed by a manual cmake configure/build.
    """

    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {"run_tests": False}

    def requirements(self):
        # Version ranges resolve against whatever the workflow's
        # `conan create` step put into the local cache earlier.
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        # FastDDS-backed provider (--provider fastdds). Always linked in:
        # the gateway ships both providers and selects at runtime.
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        # Boost.Beast / Boost.Asio for WebSocket transport.
        self.requires("boost/1.83.0")
        # nlohmann_json for WS JSON control frames.
        self.requires("nlohmann_json/3.11.3")
        if self.options.run_tests:
            self.test_requires("gtest/1.17.0")

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
