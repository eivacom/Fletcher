# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import cmake_layout


class PubSubArrowFastDdsIntegrationConan(ConanFile):
    """Cross-component integration test consumer.

    Drives a real fletcher-pubsub-arrow adapter on top of a real
    fletcher-fastdds-pubsub-provider and verifies that ArrowRow
    instances round-trip correctly across the adapter + DDS path —
    something the unit tests of each component only validate against
    mocks.

    Not published as a Conan package — this conanfile only exists so
    `conan install` resolves the right deps and writes a CMake toolchain.
    The components themselves are expected to be in the local Conan
    cache (built earlier in the workflow via `conan create <component>/.`).
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges resolve to whatever the workflow's `conan create`
        # just put in the local cache. include_prerelease is needed
        # because component versions are alpha-suffixed.
        self.requires("fletcher-pubsub-arrow/[*, include_prerelease]")
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        self.requires("gtest/1.17.0")
        # arrow pins zlib/1.2.13, openssl pulls 1.3.1 — same conflict
        # arrow-bridge handles in its own conanfile.
        self.requires("zlib/1.3.1", override=True)

    def layout(self):
        cmake_layout(self)
