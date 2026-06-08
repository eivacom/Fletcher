# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class GatewayFastddsTsIntegrationConan(ConanFile):
    """Integration-test consumer: gateway (FastDDS provider) + a C++ FastDDS
    peer + gateway-client-ts.

    Builds the production `gateway` exe via add_subdirectory (gateway has no
    Conan package), so this conanfile must provide gateway's full dependency
    set — including fletcher-fastdds-pubsub-provider, which the gateway now
    always links. It also builds a small C++ FastDDS peer from a
    protoc-gen-fletcher generated header, so fletcher-protoc + protobuf are
    needed for code generation.

    Not published as a Conan package — this conanfile only resolves deps and
    writes a CMake toolchain. The fletcher-* components are expected to be in
    the local Conan cache (created earlier in the workflow).
    """

    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        # Version ranges resolve against whatever the workflow's
        # `conan create` step has put in the local cache.
        self.requires("fletcher-pubsub/[*, include_prerelease]")
        self.requires("fletcher-core/[*, include_prerelease]")
        self.requires("fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        # protoc-gen-fletcher plugin + protobuf for generating the C++ and TS
        # row/schema/topic artefacts from sensor_reading.proto.
        self.requires("fletcher-protoc/[*, include_prerelease]")
        self.requires("protobuf/3.21.12")
        # Boost.Beast / Boost.Asio + nlohmann_json are gateway's own deps,
        # required because we build the gateway from source here.
        self.requires("boost/1.83.0")
        self.requires("nlohmann_json/3.11.3")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        # `conan build .` dispatches the right cmake configure/build for the
        # generator in use (single-config presets on Linux, multi-config on
        # Windows), avoiding the preset asymmetry of calling cmake directly.
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
