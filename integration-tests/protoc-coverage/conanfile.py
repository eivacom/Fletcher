# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
import os

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class ProtocCoverageIntegrationConan(ConanFile):
    """GIR-1 generator compile-and-run coverage harness.

    Greenfield companion to protoc-arrow-bridge: one broad `coverage.proto`
    is run through the fletcher-protoc plugin and every generated surface
    (edge row, Arrow view / ToArrowRow, IPC schema, RBA C++ accessor) is
    compiled and executed as a refactor guard before the IR rewrite.

    Not published as a Conan package — this conanfile resolves the right
    deps, writes the CMake toolchain, and drives the build via `conan build .`.
    The components (protoc, arrow-bridge, pubsub) are expected in the local
    Conan cache (built earlier via `conan create <component>/.`); Conan
    resolves locally first, so the branch's in-flight versions are used.
    """
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges so the test picks up whatever the branch's components
        # have just been built as. `include_prerelease` is needed because our
        # component versions are alpha-suffixed (e.g. 0.4.1-alpha) and Conan
        # excludes pre-releases from `[*]` by default.
        self.requires("fletcher-protoc/[*, include_prerelease]")
        self.requires("fletcher-arrow-bridge/[*, include_prerelease]")
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        # protobuf is required directly because CMakeLists invokes
        # `protobuf::protoc` to drive code generation. The protoc plugin
        # package depends on protobuf too, but Conan does not propagate that
        # as a CMake target to downstream consumers.
        self.requires("protobuf/3.21.12")
        self.requires("gtest/1.17.0")
        # arrow pins zlib/1.2.13, openssl pulls 1.3.1 — same conflict
        # arrow-bridge handles in its own conanfile.
        self.requires("zlib/1.3.1", override=True)

    def layout(self):
        cmake_layout(self)

    def build(self):
        # `conan build .` configures + builds + runs ctest through the active
        # profile, so the same call works on the Linux single-config and the
        # Windows multi-config (MSVC) generators. CTEST_OUTPUT_ON_FAILURE makes
        # cmake.test() surface failing-test output.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        os.environ["CTEST_OUTPUT_ON_FAILURE"] = "1"
        cmake.test()
