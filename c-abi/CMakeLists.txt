# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-c-abi LANGUAGES C CXX)

find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-xrcedds-pubsub-provider CONFIG REQUIRED)

# The implementation behind the ABI, as an OBJECT library.
#
# It exists because of a tension the shim's own shape creates: everything inside
# the shim is deliberately HIDDEN (the export table is exactly what binding.h
# declares, and CI asserts it on Linux), but the codec's tests have to reach the
# codec. Compiling the sources into the test executable as well would work and
# would also mean the tests exercise a second compilation of the code that ships.
# One object library, linked into both, keeps it one compilation.
#
# PIC explicitly: these objects go into a SHARED library, and unlike the
# component's own targets an object library does not inherit that requirement
# from what links it.
add_library(fletcher-c-abi-impl OBJECT
    src/nanoarrow_codec.cpp)

target_compile_features(fletcher-c-abi-impl PUBLIC cxx_std_20)

set_target_properties(fletcher-c-abi-impl PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# PUBLIC, unlike the shim's own PRIVATE links: whatever links these objects needs
# their headers too, and the test executable is one of those.
target_link_libraries(fletcher-c-abi-impl PUBLIC
    fletcher-pubsub::fletcher-pubsub)

# SHARED, unlike every other Fletcher component: this is the artifact
# `Eiva.Fletcher.Interop` ships per RID in runtimes/<rid>/native/, and P/Invoke
# needs a loadable module, not an archive.
# The entry points live HERE and not in the object library above, for one
# concrete reason: `FL_ABI_EXPORT` reads `FLETCHER_C_ABI_BUILDING`, which is
# defined on this target only, so a translation unit compiled into the object
# library would see `__declspec(dllimport)` and try to DEFINE an imported
# function. It is also the better boundary to test across — the suite reaches
# these through the shim's own export table, which is the surface that ships,
# rather than through a second compilation of them.
add_library(fletcher-c-abi SHARED
    src/binding_version.c
    src/builtins.cpp
    src/containment.cpp
    src/binding.cpp)

# The public header is C99; the implementation behind it is C++20 because the
# seam is. Both are stated: the C standard is what BIND-1's
# CompilesAsC99AndIsSelfContained test holds the header to.
target_compile_features(fletcher-c-abi PRIVATE c_std_99 cxx_std_20)

# Hide everything that is not explicitly exported, so "the shim exports only
# fl_binding_abi_version" is enforced by the build rather than asserted. On
# Windows nothing leaves without __declspec(dllexport) anyway; the two settings
# below are what make gcc/clang behave the same way, and they also stop the
# statically linked Fast DDS / Micro XRCE-DDS symbols from becoming part of the
# shim's interface (they would otherwise be interposable, and a host process
# that also links eProsima would get one of the two at random).
set_target_properties(fletcher-c-abi PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_compile_definitions(fletcher-c-abi PRIVATE FLETCHER_C_ABI_BUILDING)

# ...and the half of the same rule that visibility settings cannot reach on
# gcc/clang: CXX_VISIBILITY_PRESET governs only THIS target's own translation
# units, while symbols arriving from a static archive keep the visibility they
# were compiled with — and the eProsima chain is linked in statically.
# `--exclude-libs ALL` localises every symbol that came out of an archive, which
# is exactly the set at issue. It matters beyond tidiness: a host process that
# also links its own Fast DDS would otherwise have two definitions of every
# eProsima symbol visible at once, and the dynamic linker picks one per symbol,
# silently.
#
# WINDOWS IS NOT SOLVED, and cannot be from here (BIND-0 finding, measured):
# ConanCenter's `fast-dds` static build is compiled with EPROSIMA_USER_DLL_EXPORT,
# so `libfastdds-3.4.lib` carries 303003 /EXPORT directives in its objects'
# .drectve sections. A linker honours those when it pulls the objects into a DLL,
# and a module-definition file does NOT suppress them — MSVC merges /DEF exports
# with __declspec(dllexport) ones (tried, measured: 3531 exported names either
# way). Consequences are bounded — Windows resolves imports per-module, so there
# is no ELF-style interposition, and P/Invoke finds `fl_binding_abi_version` by
# name regardless — but the export table is bloat the packed-size budget pays
# for, and the "exports only what binding.h declares" property holds on Linux
# only. Fixing it means getting the upstream recipe to stop defining the macro
# for static builds. Revisit at BIND-9 with the size budget in hand.
#
# ...and the half that NEITHER of those reaches, found by BIND-2c's first CI run:
# libstdc++ gives a handful of `shared_ptr` internals EXPLICIT default visibility
# (typeinfo for the control block, the `_Sp_make_shared_tag` identity) so that
# RTTI works across a shared object boundary. An explicit attribute beats a
# -fvisibility default, so one `std::make_shared` in one of our own files exports
# all six regardless of the properties above. A version script is the only thing
# that settles it, and it makes "exports exactly what binding.h declares" a
# property of the LINK rather than something CI notices afterwards. The reasoning,
# and why localising them is safe, is in the map file itself.
if(NOT MSVC)
    target_link_options(fletcher-c-abi PRIVATE
        "LINKER:--exclude-libs,ALL"
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map")

    # So that editing the map relinks. Without it the map is invisible to the
    # build graph and a change to it appears to do nothing.
    set_target_properties(fletcher-c-abi PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map)
endif()

target_include_directories(fletcher-c-abi
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# PRIVATE, all three: no Fletcher C++ type appears in binding.h, so no consumer
# of the shim ever needs a Fletcher header, an Arrow header or an eProsima
# header. That is the property the test_package checks by compiling a C program
# against the package with nothing else on its include path.
target_link_libraries(fletcher-c-abi PRIVATE
    fletcher-c-abi-impl
    fletcher-pubsub::fletcher-pubsub
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
