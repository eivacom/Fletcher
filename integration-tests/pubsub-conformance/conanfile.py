# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
import os

from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class PubsubConformanceIntegrationConan(ConanFile):
    """The pub/sub delivery contract, run against every provider.

    docs/pubsub-interface-spec.md §7 says a callback never sees a null
    schema, per-writer order holds across the schema handoff, and no
    callback runs after Unsubscribe returns. That was prose honoured by
    three providers under review; this harness encodes it once and runs
    it against five subjects — the in-process loopback plus Fast DDS and
    XRCE-DDS, each in-process AND with the publisher in a child process.

    The cross-process subjects are the point (§7.2): Fast DDS serves
    same-process endpoints over intra-process delivery, so a
    single-process suite cannot see the transport at all. The provider's
    own 70-test suite was green throughout a shipped receive-side
    data-sharing defect for exactly that reason.

    Deliberately does NOT require fletcher-pubsub-arrow: no Arrow C++,
    no codec, no generated types. Rows are 8 opaque bytes, so the suite
    cannot see payload layout and no divergence it forces can be a
    wire-format change (locked decision 13).

    Not published as a Conan package — this conanfile resolves the deps,
    writes the CMake toolchain, and drives the build. The components
    themselves are expected to be in the local Conan cache (built
    earlier in the workflow via `conan create <component>/.`).
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges resolve to whatever the workflow's `conan create`
        # just put in the local cache. include_prerelease is needed
        # because component versions are alpha-suffixed.
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        self.requires("fletcher-xrcedds-pubsub-provider/[*, include_prerelease]")
        self.requires("gtest/1.17.0")

    def layout(self):
        cmake_layout(self)

    def build(self):
        # `conan build .` configures + builds (including the
        # MicroXRCEAgent ExternalProject) + runs ctest through the active
        # profile, so the same call works on the Linux single-config and
        # the Windows multi-config (MSVC) generators.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        os.environ["CTEST_OUTPUT_ON_FAILURE"] = "1"
        cmake.test()
