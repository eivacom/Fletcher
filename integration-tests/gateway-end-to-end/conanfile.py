# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import cmake_layout


class GatewayEndToEndIntegrationConan(ConanFile):
    """Integration-test consumer for the gateway + gateway-client-ts pair.

    Builds a C++ test_server binary that wraps the WebGateway with an
    in-process mock provider. The accompanying vitest suite (in test/)
    spawns the binary and exercises the WebSocket protocol via the
    real FletcherClient from gateway-client-ts.

    Boost + nlohmann_json are pulled in because the gateway source is
    compiled inline (gateway has no published Conan package); pubsub +
    core are expected to be present in the local cache via a prior
    `conan create core/.` + `conan create pubsub/.`.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges resolve against whatever the workflow's
        # `conan create` step has put in the local cache.
        self.requires("eiva-fletcher-pubsub/[*, include_prerelease]")
        self.requires("eiva-fletcher-core/[*, include_prerelease]")
        # Boost.Beast / Boost.Asio for the inline gateway build.
        self.requires("boost/1.83.0")
        # nlohmann_json for the gateway's JSON control frames.
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)
