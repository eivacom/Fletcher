# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherCAbiConan(ConanFile):
    """The BINDING ABI shim: Fletcher behind one pure C surface.

    This is the only Fletcher component packaged as a SHARED library. It is the
    native asset `Eiva.Fletcher.Interop` ships per RID (runtimes/<rid>/native/),
    and it is what every language binding — C# this round, Rust the next — calls
    Fletcher through. Nothing above it links a Fletcher C++ library.

    The shim statically links and registers the three built-in providers
    (`inprocess`, `fastdds`, `xrce`), so one file per RID is the whole native
    deployment: no loader, no driver binaries beside it, no conanrun.
    """

    name = "fletcher-c-abi"
    version = "0.5.0-alpha"
    description = "Pure C binding ABI over Fletcher, shipped as the bindings' native shim"
    license = "LGPL-3.0-or-later"
    package_type = "shared-library"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {
        "run_tests": False,
        # Inherited in spirit from the fastdds-pubsub-provider recipe, and
        # restated here because THIS is the artifact the decision is about: the
        # eProsima chain is linked statically INTO the shim, so the NuGet package
        # carries one native file per RID.
        "fast-dds/*:shared": False,
        "fast-cdr/*:shared": False,
        "foonathan-memory/*:shared": False,
        "tinyxml2/*:shared": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "README.md",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
    )

    def requirements(self):
        # NO transitive_headers anywhere in this recipe, on purpose: binding.h
        # names no Fletcher, Arrow or eProsima type, so a consumer of this
        # package needs exactly one header and no include path but its own.
        # test_package compiles a C program against the package to keep that
        # claim a test rather than a comment.
        self.requires("fletcher-pubsub/0.5.0-alpha")
        self.requires("fletcher-fastdds-pubsub-provider/0.5.0-alpha")
        self.requires("fletcher-xrcedds-pubsub-provider/0.5.1-alpha")
        if self.options.run_tests:
            self.requires("gtest/1.17.0")

    def package_id(self):
        del self.info.options.run_tests

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
            # cmake.test() activates VirtualRunEnv, so the shim itself and any
            # dependency DLLs are on PATH when the test executables launch.
            cmake.test()

    def package(self):
        copy(self, "*.h",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        # The loadable module goes to bin/ on Windows (a DLL is a runtime
        # artifact there) and lib/ elsewhere; the MSVC import library goes to
        # lib/ either way. `Eiva.Fletcher.Interop`'s packaging step picks the
        # loadable module up from whichever of the two it lands in.
        # The patterns keep a leading wildcard (Conan matches them against the
        # path RELATIVE to src, so a bare "fletcher-c-abi.lib" matches nothing)
        # but name the file: the tests build a C99 probe object library beside
        # the shim, and a plain "*.lib" packages that too.
        copy(self, "*fletcher-c-abi.dll",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "bin"),
             keep_path=False)
        copy(self, "*libfletcher-c-abi.so*",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*fletcher-c-abi.lib",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["fletcher-c-abi"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "fletcher-c-abi")
        self.cpp_info.set_property("cmake_target_name", "fletcher-c-abi::fletcher-c-abi")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-c-abi-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
