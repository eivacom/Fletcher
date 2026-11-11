# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class GatewayDotnetEndToEndIntegrationConan(ConanFile):
    """Integration-test consumer for gateway + Eiva.Fletcher.GatewayClient (BIND-8b).

    Builds the production `gateway` exe, exactly as integration-tests/gateway-end-to-end does
    for the TypeScript client: gateway/CMakeLists.txt comes in through add_subdirectory, since
    the gateway is never a Conan package. The C# test project in dotnet/ spawns the resulting
    exe and drives it with the real FletcherClient; it generates its C# model from proto/ with
    the fletcher-protoc plugin itself (D-BIND-71's consumer recipe), so nothing here runs
    protoc.

    pubsub, core and fastdds-pubsub-provider are expected in the local Conan cache via a prior
    `conan create` of each.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        # The gateway always links the Fast DDS provider and selects one at runtime.
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        # Boost.Beast / Boost.Asio for the WebSocket transport; nlohmann_json for its control frames.
        self.requires("boost/1.83.0")
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
