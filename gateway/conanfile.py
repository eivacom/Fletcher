# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import load


class GatewayConan(ConanFile):
    """Conan package for the gateway application.

    The gateway is distributed as a self-contained bundle — the executable plus
    the runtime shared libraries it links (Fast DDS et al.) beside it, so it
    runs with zero setup (no LD_LIBRARY_PATH / conanrun). It is an `application`
    package: no headers, no consumable library.

    `conan create .` builds and packages the exe with an $ORIGIN rpath; the
    bundle is then produced entirely with Conan, e.g.

        conan create . -pr:a=<profile>
        conan install --requires=fletcher-gateway/<version> \\
            --deployer=runtime_deploy --deployer-folder=dist -pr:a=<profile>

    which flattens the exe and every runtime dependency into `dist/`. The plain
    `conan install . && conan build .` dev/test loop still works too.
    """

    name = "fletcher-gateway"
    package_type = "application"
    license = "LGPL-3.0-or-later"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {"run_tests": False}

    exports_sources = "CMakeLists.txt", "src/*", "cmake/*", "tests/*", "VERSION"

    def set_version(self):
        # Single source of truth for the version is gateway/VERSION (the same
        # file CMake reads for project(VERSION ...) and the --version flag).
        self.version = load(self, os.path.join(self.recipe_folder, "VERSION")).strip()

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

    def package_id(self):
        # run_tests only affects the build/test loop, not the shipped exe.
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
        # Installs the exe (with its $ORIGIN rpath) per the CMakeLists
        # install() rule. runtime_deploy then places the runtime libs beside it.
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        # Application package: the exe sits at the package root (install
        # DESTINATION .); nothing to consume as a library.
        self.cpp_info.bindirs = ["."]
        self.cpp_info.libdirs = []
        self.cpp_info.includedirs = []
