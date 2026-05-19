# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.20)
project(eiva-fletcher-xrcedds-pubsub-provider CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ── Micro-CDR (dependency of XRCE-DDS Client) ───────────────────────────
# Disable SuperBuild in both Micro-CDR and XRCE-DDS Client: their
# ExternalProject paths break when Conan sets a relative toolchain path.
set(UCDR_SUPERBUILD OFF CACHE BOOL "" FORCE)
set(UCDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UCDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UCDR_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    microcdr
    GIT_REPOSITORY https://github.com/eProsima/Micro-CDR.git
    GIT_TAG        v2.0.1)
FetchContent_MakeAvailable(microcdr)

# Make microcdr findable by XRCE-DDS Client's find_package() call.
# FetchContent creates the target but not a *Config.cmake file.
# We point microcdr_DIR to the build dir and write a shim config file.
set(_microcdr_config_dir "${CMAKE_BINARY_DIR}/_deps/microcdr-config")
file(MAKE_DIRECTORY "${_microcdr_config_dir}")
file(WRITE "${_microcdr_config_dir}/microcdrConfig.cmake"
    "# Shim: microcdr is already available via FetchContent.\n"
    "if(NOT TARGET microcdr)\n"
    "  message(FATAL_ERROR \"microcdr target not found\")\n"
    "endif()\n"
    "set(microcdr_FOUND TRUE)\n"
    "set(microcdr_VERSION 2.0.1)\n")
file(WRITE "${_microcdr_config_dir}/microcdrConfigVersion.cmake" [=[
set(PACKAGE_VERSION "2.0.1")
if("${PACKAGE_FIND_VERSION}" VERSION_EQUAL "2.0.1")
  set(PACKAGE_VERSION_EXACT TRUE)
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
  set(PACKAGE_VERSION_UNSUITABLE FALSE)
else()
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
  set(PACKAGE_VERSION_UNSUITABLE TRUE)
endif()
]=])
set(microcdr_DIR "${_microcdr_config_dir}" CACHE PATH "" FORCE)
list(APPEND CMAKE_PREFIX_PATH "${_microcdr_config_dir}")

# ── Micro XRCE-DDS Client ───────────────────────────────────────────────
set(UCLIENT_SUPERBUILD OFF CACHE BOOL "" FORCE)
set(UCLIENT_BUILD_MICROCDR OFF CACHE BOOL "" FORCE)
set(UCLIENT_PROFILE_UDP ON CACHE BOOL "" FORCE)
set(UCLIENT_PROFILE_TCP ON CACHE BOOL "" FORCE)
set(UCLIENT_PROFILE_SERIAL ON  CACHE BOOL "" FORCE)
set(UCLIENT_PROFILE_DISCOVERY OFF CACHE BOOL "" FORCE)
set(UCLIENT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(UCLIENT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UCLIENT_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    microxrcedds_client
    GIT_REPOSITORY https://github.com/eProsima/Micro-XRCE-DDS-Client.git
    GIT_TAG        v2.4.3)
FetchContent_MakeAvailable(microxrcedds_client)

# ── Fletcher dependencies ────────────────────────────────────────────────
find_package(eiva-fletcher-pubsub REQUIRED)
find_package(eiva-fletcher-core REQUIRED)

# ── Provider library ─────────────────────────────────────────────────────
add_library(xrce_dds_pubsub_provider
    src/xrce_dds_pubsub_provider.cpp)
target_include_directories(xrce_dds_pubsub_provider PUBLIC include)
target_link_libraries(xrce_dds_pubsub_provider
    PUBLIC  eiva-fletcher-pubsub::eiva-fletcher-pubsub
    PRIVATE microxrcedds_client)

if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
