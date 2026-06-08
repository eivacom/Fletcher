# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class GatewayEndToEndIntegrationConan(ConanFile):
    """Integration-test consumer for gateway + gateway-client-ts.

    Builds the production `gateway` exe. Gateway has its own
    conanfile.py but is never uploaded as a Conan package (it has no
    name/version); for the integration test, we pull
    `gateway/CMakeLists.txt` in via add_subdirectory rather than going
    through Conan. The accompanying vitest suite spawns the resulting
    exe and exercises the WebSocket protocol via the real
    FletcherClient from gateway-client-ts.

    Boost, nlohmann_json, and yaml-cpp are required by gateway's
    sources. pubsub + core are expected to be present in the local
    Conan cache via a prior `conan create core/.` + `conan create
    pubsub/.`.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges resolve against whatever the workflow's
        # `conan create` step has put in the local cache.
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        # gateway now always links the FastDDS provider (it ships both
        # providers and selects at runtime), so building the gateway via
        # add_subdirectory pulls this in.
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        # protoc-gen-fletcher plugin + protobuf for generating the TS
        # row class used by the proto-gen test case.
        self.requires("fletcher-protoc/[*, include_prerelease]")
        self.requires("protobuf/3.21.12")
        # Boost.Beast / Boost.Asio for gateway's WebSocket transport.
        self.requires("boost/1.83.0")
        # nlohmann_json for the gateway's WS JSON control frames.
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)

    def build(self):
        # `conan build .` configures + builds the gateway exe through the
        # active profile: Conan picks the generator and the matching
        # configure/build preset, so the same call works on the Linux
        # single-config and the Windows multi-config (MSVC) generators.
        # There are no CMake-level tests here — the cross-language suite
        # spawns the exe from a separate `npm test` step.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
