# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.errors import ConanException
from conan.tools.files import copy, save
import json
import os

# The Fast DDS this provider links. BIND-9b-7 (D-BIND-166): the code Fast DDS compiles in from its own
# thirdparty/ folder is not in the Conan graph, so its licence texts are carried in
# third_party/fast-dds-<version>/ and packaged for the notice deployer; a different version has no
# such folder and fails package(), so an upgrade re-checks what the new Fast DDS vendors.
FAST_DDS_VERSION = "3.4.3"

# Name, version, licence and homepage of each library in that folder, read off Fast DDS 3.4.3's
# thirdparty/ (versions from the vendored headers). Only what src/cpp compiles in: optionparser
# serves its tools and android-ifaddrs Android, neither in this library.
FAST_DDS_VENDORED = {
    "nlohmann-json": ("3.11.3", "MIT", "https://github.com/nlohmann/json"),
    "filewatch": ("as vendored by fast-dds " + FAST_DDS_VERSION, "MIT",
                  "https://github.com/ThomasMonkman/filewatch"),
    # 178 of the 181 files point to PEGTL 2.8.3's LICENSE (MIT); pegtl.hpp and the two mmap
    # headers say BSL-1.0, as Fast DDS updated them: both texts are carried.
    "taocpp-pegtl": ("2.8.3", "MIT AND BSL-1.0", "https://github.com/taocpp/PEGTL"),
    # Header-only Boost.Interprocess and Boost.Thread time, for the shared-memory transport.
    "boost": ("1.73.0", "BSL-1.0", "https://www.boost.org"),
}


class FletcherPubsubConan(ConanFile):
    name = "fletcher-fastdds-pubsub-provider"
    version = "0.5.1-alpha"
    description = "Fletcher FastDDS PubSub Provider library"
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
    default_options = {
        "run_tests": False,
        "fPIC": True,
        # Link the Fast DDS chain statically by default. This recipe owns the
        # fast-dds requirement, so the static default lives here rather than in
        # a blanket *:shared profile option — consumers (notably the gateway)
        # then bake Fast DDS into a self-contained binary that runs with no
        # conanrun / LD_LIBRARY_PATH. Opt back into shared by layering the
        # `shared` profile fragment (a higher-priority profile option wins).
        "fast-dds/*:shared": False,
        "fast-cdr/*:shared": False,
        "foonathan-memory/*:shared": False,
        "tinyxml2/*:shared": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        # README.md is exported for the TESTS, not for documentation: B1's
        # `DefaultProfileTranscriptionIsExact` reads the published starting-point XML block out
        # of it, so the cache build must have the file the repository holds.
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
        # NO transitive_headers: the public header names no eProsima type (PDA-DEC-6), so a
        # consumer configures this provider through ProviderConfig and an XML profiles document
        # without ever seeing a Fast DDS header. Dropping it is what makes test_package a real
        # check of that claim rather than a claim. transitive_libs STAYS -- the library is
        # STATIC, so consumers still link the Fast DDS chain.
        self.requires(f"fast-dds/{FAST_DDS_VERSION}", transitive_libs=True)
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
        # The code Fast DDS compiles in from its own thirdparty/ folder (D-BIND-166), as
        # fletcher-pubsub packages its vendored nanoarrow: the texts travel here for the notice
        # deployer (gateway/deployers/third_party_licenses.py), which names Fast DDS as the
        # package that compiles them in.
        texts = os.path.join(self.source_folder, "third_party", f"fast-dds-{FAST_DDS_VERSION}")
        if not os.path.isdir(texts):
            raise ConanException(
                f"no {texts}: re-check which third-party code fast-dds {FAST_DDS_VERSION} compiles in "
                "from its thirdparty/ folder, and carry those licence texts there")
        for name, (version, licence, homepage) in FAST_DDS_VENDORED.items():
            vendored = os.path.join(self.package_folder, "licenses", "vendored", name)
            if not copy(self, "*", src=os.path.join(texts, name), dst=vendored):
                raise ConanException(f"{texts}/{name} carries no licence text")
            save(self, os.path.join(vendored, "vendored.json"), json.dumps({
                "name": name, "version": version, "license": licence, "homepage": homepage,
                "via": f"fast-dds {FAST_DDS_VERSION}"}, indent=2))

    def package_info(self):
        self.cpp_info.libs = ["fletcher-fastdds-pubsub-provider"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "fletcher-fastdds-pubsub-provider")
        self.cpp_info.set_property("cmake_target_name", "fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-fastdds-pubsub-provider-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
