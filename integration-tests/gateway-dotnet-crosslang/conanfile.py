# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class GatewayDotnetCrosslangIntegrationConan(ConanFile):
    """Integration-test consumer: the cross-language rows of the C# gateway client (BIND-8d).

    Builds the production `gateway` exe via add_subdirectory (gateway has no Conan package), so
    this conanfile provides gateway's full dependency set, plus the C++ peer's: the generated
    native pair over the Fast DDS provider, and the plugin + protobuf to generate it. The C#
    native peer's shim (fletcher-c-abi) is built by the workflow separately and handed to
    `dotnet test` as -p:FletcherNativeShim.

    Not published as a Conan package; it only resolves deps and writes a CMake toolchain. The
    fletcher-* components are expected in the local Conan cache.
    """

    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        self.requires("fletcher-protoc/[*, include_prerelease]")
        self.requires("protobuf/3.21.12")
        # The gateway's own deps, because it is built from source here.
        self.requires("boost/1.83.0")
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
