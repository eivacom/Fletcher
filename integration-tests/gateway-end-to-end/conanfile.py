# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import cmake_layout


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
        self.requires("eiva-fletcher-pubsub/[*, include_prerelease]")
        self.requires("eiva-fletcher-core/[*, include_prerelease]")
        # protoc-gen-fletcher plugin + protobuf for generating the TS
        # row class used by the proto-gen test case.
        self.requires("eiva-fletcher-protoc/[*, include_prerelease]")
        self.requires("protobuf/3.21.12")
        # Boost.Beast / Boost.Asio for gateway's WebSocket transport.
        self.requires("boost/1.83.0")
        # nlohmann_json for the gateway's WS JSON control frames.
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)
