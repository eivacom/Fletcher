from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy
import os


class FletcherCoreConan(ConanFile):
    name = "eiva-fletcher-core"
    version = "0.1.2-alpha"
    description = "EIVA Fletcher Core library"
    license = "Proprietary"
    package_type = "header-library"
    settings = "os", "compiler", "build_type", "arch"  # needed for test builds only

    options = {"run_tests": [True, False]}
    default_options = {"run_tests": False}

    exports_sources = (
        "CMakeLists.txt",
        "include/*",
        "cmake/*",
        "tests/*",
    )

    def requirements(self):
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
        copy(self, "*.cmake",
             src=os.path.join(self.source_folder, "cmake"),
             dst=os.path.join(self.package_folder, "cmake"),
             keep_path=False)

    def package_info(self):
        self.cpp_info.includedirs = ["include"]
        self.cpp_info.libdirs     = []
        self.cpp_info.bindirs     = []
        self.cpp_info.set_property("cmake_file_name", "eiva-fletcher-core")
        self.cpp_info.set_property("cmake_target_name", "eiva-fletcher-core::eiva-fletcher-core")
        self.cpp_info.set_property("cmake_build_modules", [
            os.path.join("cmake", "fletcher-core-target.cmake"),
        ])
        self.cpp_info.builddirs = ["cmake"]
