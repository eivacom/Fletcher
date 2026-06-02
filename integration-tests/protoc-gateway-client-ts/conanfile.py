# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class ProtocGatewayClientTsIntegrationConan(ConanFile):
    """Cross-language integration test consumer.

    Builds a C++ vector emitter that uses protoc-generated row classes to
    encode known scenarios. A TypeScript vitest test then spawns the
    binary, decodes the bytes via the generated TS codec, and asserts
    cross-language byte-compatibility.

    `conan build .` runs configure + cmake build to produce the emitter
    binary; the cross-language assertions run in a separate `npm test`
    step.

    The components themselves (protoc, core, pubsub, etc.) are expected
    to be in the local Conan cache (built earlier in the workflow via
    `conan create <component>/.`).
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges resolve to whatever the workflow's `conan create`
        # just put in the local cache. include_prerelease is needed
        # because component versions are alpha-suffixed.
        self.requires("fletcher-protoc/[*, include_prerelease]")
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        self.requires("protobuf/3.21.12")

    def layout(self):
        cmake_layout(self)

    def build(self):
        # No CMake-level tests — the emit_vectors binary is the build
        # output, and the cross-language assertions live in vitest
        # (spawned by a separate `npm test` step).
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
