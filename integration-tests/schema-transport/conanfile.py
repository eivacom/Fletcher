from conan import ConanFile
from conan.tools.cmake import cmake_layout


class SchemaTransportIntegrationConan(ConanFile):
    """Cross-component integration test consumer.

    Drives the FastDDSPubSubProvider's `/__schema` companion topic
    machinery through three timing scenarios that the per-provider
    unit tests don't cover: late-joining subscribers, publisher
    restart within a domain lifetime, and subscribers that connect
    before the publisher has created the topic.

    Not published as a Conan package — this conanfile only exists so
    `conan install` resolves the right deps and writes a CMake toolchain.
    The components themselves are expected to be in the local Conan
    cache (built earlier in the workflow via `conan create <component>/.`).
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Version ranges resolve to whatever the workflow's `conan create`
        # just put in the local cache. include_prerelease is needed
        # because component versions are alpha-suffixed.
        self.requires("eiva-fletcher-pubsub-arrow/[*, include_prerelease]")
        self.requires("eiva-fletcher-fastdds-pubsub-provider/[*, include_prerelease]")
        self.requires("gtest/1.17.0")
        # arrow pins zlib/1.2.13, openssl pulls 1.3.1 — same conflict
        # arrow-bridge handles in its own conanfile.
        self.requires("zlib/1.3.1", override=True)

    def layout(self):
        cmake_layout(self)
