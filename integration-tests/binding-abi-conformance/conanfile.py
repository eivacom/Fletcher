# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class BindingAbiConformanceConan(ConanFile):
    """The C++ half of the binding ABI conformance suite.

    Builds `emit_corpus`, which writes the codec's own fixture corpus to a
    directory in two forms: each batch as an Arrow IPC stream, and the same rows
    as the SHIM encoded them through `fl_codec_open` / `fl_rows_bind` /
    `fl_encode_row`.

    A `dotnet test` step then rebuilds those batches with `Apache.Arrow`, exports
    them across the C Data Interface, encodes them through the same shim, and
    requires the bytes to match. That is the question the suite exists to answer:
    two independent Arrow implementations lay out the same logical batch, and the
    codec must not be able to tell them apart. If it can, the difference reaches
    the wire as a subscriber decoding a publisher's row into the wrong values.

    The components (fletcher-c-abi and its chain) are expected to be in the local
    Conan cache, built earlier in the workflow with `conan create <component>/.`,
    exactly as the other integration tests expect theirs.
    """

    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # include_prerelease because the component versions are alpha-suffixed.
        self.requires("fletcher-c-abi/[*, include_prerelease]")
        # Arrow C++ is the emitter's own dependency and NOT the shim's: the shim
        # links nanoarrow only, which is most of the reason the codec was written
        # on nanoarrow at all. Pinned to the version the components use.
        self.requires("arrow/23.0.1")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
