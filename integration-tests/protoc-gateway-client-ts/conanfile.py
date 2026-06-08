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
        # `conan build .` configures + builds the emit_vectors exe through
        # the active profile (Conan picks the generator + matching preset,
        # so this works the same on Linux single-config and Windows
        # multi-config). The vitest byte-compat suite spawns the exe from
        # a separate `npm test` step.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
