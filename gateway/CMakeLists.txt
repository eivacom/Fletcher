# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Boost.Beast and Boost.Asio are header-only.  The Conan Boost recipe
# does not populate per-component INCLUDE_DIRS, so we locate the headers
# via find_path (the Conan cache is already on CMAKE_PREFIX_PATH).
find_path(BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)

add_library(web_gateway
    src/web_gateway.cpp
    src/ws_session.cpp)
target_include_directories(web_gateway
    PUBLIC  include
    PRIVATE src ${BOOST_INCLUDE_DIR})
target_link_libraries(web_gateway PUBLIC pubsub PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(web_gateway PRIVATE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00)   # Windows 10+

add_executable(gateway_example examples/gateway_main.cpp)
target_link_libraries(gateway_example PRIVATE web_gateway)
target_compile_definitions(gateway_example PRIVATE
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00)
