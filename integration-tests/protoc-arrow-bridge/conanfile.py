from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout


class ProtocArrowBridgeIntegrationConan(ConanFile):
    """Cross-component integration test consumer.

    Not published as a Conan package — this conanfile only exists so that
    `conan install` resolves the right deps and writes a CMake toolchain.

    The components themselves (protoc, arrow-bridge, etc.) are expected to
    be in the local Conan cache (built earlier in the workflow via
    `conan create <component>/.`). Conan resolves locally first, so the
    branch's in-flight versions are used — we never reach for the
    conan-eiva remote for our own packages.
    """
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges so the test picks up whatever the branch's
        # components have just been built as. The workflow runs
        # `conan create <component>/.` before us, putting the branch's
        # current version in the local cache; the range resolves to that.
        # `include_prerelease` is needed because our component versions
        # are alpha-suffixed (e.g. 0.1.0-alpha) and Conan excludes
        # pre-releases from `[*]` by default.
        self.requires("eiva-fletcher-protoc/[*, include_prerelease]")
        self.requires("eiva-fletcher-arrow-bridge/[*, include_prerelease]")
        self.requires("gtest/1.17.0")
        # arrow pins zlib/1.2.13, openssl pulls 1.3.1 — same conflict
        # arrow-bridge handles in its own conanfile.
        self.requires("zlib/1.3.1", override=True)

    def layout(self):
        cmake_layout(self)
