# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# gateway is distributed as a single executable. There is no public
# library API, no installed headers, and no Conan package: the
# WebSocket protocol is the only supported integration point. The
# CMakeLists.txt is consumed either standalone (against a Conan cache
# that has the deps below) or via add_subdirectory from a parent
# project that has already done the conan install.
#
# Boost.Beast / Boost.Asio are header-only. The Conan Boost recipe
# does not populate per-component INCLUDE_DIRS, so we locate the
# headers via find_path (the Conan cache is already on
# CMAKE_PREFIX_PATH for any consumer that ran conan install).
find_package(eiva-fletcher-pubsub CONFIG REQUIRED)
find_package(eiva-fletcher-core   CONFIG REQUIRED)
find_package(nlohmann_json        CONFIG REQUIRED)
find_path(BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)

# ─────────────────────────────────────────────────────────────────────
# gateway — the only artifact this directory produces. All sources
# are compiled directly into the executable: there is no intermediate
# library target since nothing else in Fletcher links against the
# gateway code (and we want to keep it that way).
# ─────────────────────────────────────────────────────────────────────
add_executable(gateway
    src/main.cpp
    src/gateway.cpp
    src/ws_session.cpp)
target_include_directories(gateway PRIVATE src ${BOOST_INCLUDE_DIR})
target_link_libraries(gateway PRIVATE
    eiva-fletcher-pubsub::eiva-fletcher-pubsub
    eiva-fletcher-core::eiva-fletcher-core
    nlohmann_json::nlohmann_json)
if(WIN32)
    target_compile_definitions(gateway PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)   # Windows 10+
endif()
