from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherArrowBridgeConan(ConanFile):
    name = "eiva-fletcher-arrow-bridge"
    version = "0.1.0-alpha"
    description = "EIVA Fletcher Arrow C++ bridge — Codec, ArrowRowView, CRS utilities"
    license = "Proprietary"
    package_type = "static-library"
    settings = "os", "compiler", "build_type", "arch"

    options = {"run_tests": [True, False]}
    default_options = {"run_tests": False}

    exports_sources = (
        "CMakeLists.txt",
        "src/*",
        "include/*",
        "cmake/*",
        "tests/*",
    )

    def requirements(self):
        # core types appear in arrow-bridge's public interface (codec.hpp →
        # core/types.hpp), so headers must be transitively visible.
        self.requires("eiva-fletcher-core/0.1.5-alpha", transitive_headers=True)
        self.requires("arrow/23.0.1", transitive_headers=True, transitive_libs=True)
        # Resolve a transitive zlib conflict between arrow (pins 1.2.13) and
        # openssl (range >=1.2.11 <2, resolves to 1.3.1). Pin to 1.3.1 to match
        # what conan-eiva already publishes.
        self.requires("zlib/1.3.1", override=True)
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
            cmake.test()

    def package(self):
        copy(self, "*.hpp",
             src=os.path.join(self.source_folder, "include"),
             dst=os.path.join(self.package_folder, "include"),
             keep_path=True)
        copy(self, "*.a",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.lib",
             src=self.build_folder,
             dst=os.path.join(self.package_folder, "lib"),
             keep_path=False)
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.libs = ["fletcher-arrow-bridge"]
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.set_property("cmake_file_name", "eiva-fletcher-arrow-bridge")
        self.cpp_info.set_property("cmake_target_name", "eiva-fletcher-arrow-bridge::eiva-fletcher-arrow-bridge")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-arrow-bridge-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
