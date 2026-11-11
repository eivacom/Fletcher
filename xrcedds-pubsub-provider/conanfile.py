# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.errors import ConanException
from conan.tools.files import copy, save
import json
import os
import re


class FletcherPubsubConan(ConanFile):
    name = "fletcher-xrcedds-pubsub-provider"
    version = "0.5.1-alpha"
    description = "Fletcher XRCE-DDS PubSub Provider library"
    license = "LGPL-3.0-or-later"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    # fPIC, and why a static library in this tree needs it stated.
    #
    # Round BIND adds `c-abi/`, the first component packaged as a SHARED library:
    # it links this archive into `libfletcher-c-abi.so`. Without the option Conan
    # never sets CMAKE_POSITION_INDEPENDENT_CODE, and the link fails with
    # "relocation R_X86_64_TPOFF32 against `__tls_guard' can not be used when
    # making a shared object" — found by the first Linux lane run of PR #129, on a
    # tree where every local Windows build was green.
    #
    # Deleted on Windows in config_options(): the flag has no meaning for MSVC, and
    # keeping the option there would change package IDs for no reason.
    options = {"run_tests": [True, False], "fPIC": [True, False]}
    default_options = {"run_tests": False, "fPIC": True}

    exports_sources = (
        "CMakeLists.txt",
        # README.md is exported for the TESTS, not for documentation:
        # XrceConfig.PublishedDefaultsAreExact reads the published default document out of it,
        # so the cache build must have the file the repository holds.
        "README.md",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
        "third_party/*",
    )

    def requirements(self):
        self.requires("fletcher-pubsub/0.5.1-alpha", transitive_headers=True)
        self.requires("fletcher-core/0.5.0-alpha", transitive_headers=True)
        if self.options.run_tests:
            self.requires("gtest/1.17.0")

    def package_id(self):
        del self.info.options.run_tests

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        if self.options.run_tests:
            tc.cache_variables["FLETCHER_BUILD_TESTS"] = "ON"
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if self.options.run_tests:
            # cmake.test() activates VirtualRunEnv so dependency DLLs (gtest,
            # etc.) are on PATH when the test executables launch. A bare
            # self.run("ctest ...") skips this and fails on Windows with
            # 0xC0000135 (STATUS_DLL_NOT_FOUND) under *:shared=True.
            cmake.test()

    def package(self):
        # Public pubsub headers.
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        # Built static library.
        copy(self, "*.a",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.lib",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        # CMake target alias module.
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)
        # Micro-CDR and the Micro XRCE-DDS Client (both Apache-2.0) are fetched by
        # CMakeLists.txt with FetchContent and compiled into this package, so they are
        # not in the Conan graph: their licence texts travel here for the third-party
        # notice deployer (gateway/deployers/third_party_licenses.py, BIND-9a-0,
        # D-BIND-140). The versions are the GIT_TAG pins the build fetched.
        with open(os.path.join(self.source_folder, "CMakeLists.txt"), encoding="utf-8") as handle:
            cmake = handle.read()
        for name, fetched, repo, homepage in (
                ("micro-cdr", "microcdr", "Micro-CDR", "https://github.com/eProsima/Micro-CDR"),
                ("micro-xrce-dds-client", "microxrcedds_client", "Micro-XRCE-DDS-Client",
                 "https://github.com/eProsima/Micro-XRCE-DDS-Client")):
            match = re.search(rf"{repo}\.git\s+GIT_TAG\s+v?(\S+?)\)", cmake)
            if not match:
                raise ConanException(f"no GIT_TAG for {repo} in CMakeLists.txt")
            vendored = os.path.join(self.package_folder, "licenses", "vendored", name)
            copy(self, "LICENSE", src=os.path.join(self.build_folder, "_deps", f"{fetched}-src"),
                 dst=vendored, keep_path=False)
            if not os.path.isfile(os.path.join(vendored, "LICENSE")):
                raise ConanException(f"{fetched}'s LICENSE was not fetched into {self.build_folder}/_deps")
            save(self, os.path.join(vendored, "vendored.json"), json.dumps({
                "name": name, "version": match.group(1), "license": "Apache-2.0",
                "homepage": homepage}, indent=2))

    def package_info(self):
        # The xrce_dds_pubsub_provider static library links privately
        # against microxrcedds_client (which in turn depends on microcdr),
        # both built from source via FetchContent and packaged alongside.
        # External consumers must link all three in dependency order so
        # the uxr_*/ucdr_* symbols resolve.
        self.cpp_info.libs = [
            "fletcher-xrcedds-pubsub-provider",
            "microxrcedds_client",
            "microcdr",
        ]
        self.cpp_info.includedirs = ["include"]
        # microxrcedds_client's UDP/TCP transports call straight into Winsock, so
        # a consumer linking THIS package alone (as the pub/sub conformance
        # harness deliberately does — one provider per binary) has to be given
        # ws2_32. It used to resolve by accident: every consumer also linked the
        # Fast DDS provider, whose Conan package declares ws2_32 itself.
        if self.settings.os == "Windows":
            self.cpp_info.system_libs = ["ws2_32", "iphlpapi"]
        self.cpp_info.set_property("cmake_file_name", "fletcher-xrcedds-pubsub-provider")
        self.cpp_info.set_property("cmake_target_name", "fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-xrcedds-pubsub-provider-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
