# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import load


class GatewayConan(ConanFile):
    """Conan package for the gateway application.

    The gateway is distributed as a self-contained deploy that runs with zero
    setup (no LD_LIBRARY_PATH / conanrun). It is an `application` package: no
    headers, no consumable library. How self-contained the deploy looks depends
    on how its dependencies are linked:

      * Default (static) build — the Fast DDS chain (pinned static by the
        fastdds-pubsub-provider recipe) and boost are baked in, so the deploy
        is a single self-contained executable; ldd shows only system libs.
      * Shared variant (`-pr:a=shared`) — those deps are shared, so the deploy
        is the exe plus the runtime libraries beside it, found via the $ORIGIN
        rpath the install rule sets.

    `conan create .` builds and packages the exe; the deploy is then produced
    entirely with Conan, e.g.

        conan create . -pr:a=<profile>
        conan install --requires=fletcher-gateway/<version> \\
            --deployer=runtime_deploy --deployer-folder=dist -pr:a=<profile>

    runtime_deploy flattens the exe and any runtime shared libs into `dist/`
    (just the exe for a static build). The plain `conan install . &&
    conan build .` dev/test loop still works too.
    """

    name = "fletcher-gateway"
    package_type = "application"
    license = "LGPL-3.0-or-later"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {
        "run_tests": False,
        # Pin boost static so no boost shared libs ever land in the
        # self-contained bundle, independent of the upstream default (boost is
        # header-only for us — Beast/Asio — so this only affects the few
        # compiled boost libs). The Fast DDS chain is pinned static by the
        # fastdds-pubsub-provider recipe and inherited here.
        "boost/*:shared": False,
    }

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
