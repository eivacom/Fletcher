# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# The MicroXRCEAgent the C# cross-transport suite runs its `xrce` rows against
# (D-BIND-56), and nothing else: no test binary, no Fletcher. The recipe is the
# shared one the C++ interop lane uses, so both lanes build the same Agent on the
# same Fast DDS - and on Windows land it in the same cached install dir.
cmake_minimum_required(VERSION 3.15)
project(xrce-agent-for-transport-conformance CXX)

# On Linux the shared recipe's default install dir is under the build dir, whose
# name depends on the layout; the lane needs a path it can name. Windows keeps the
# recipe's fixed C:/fl-uxa-install, which is the unit the CI cache holds.
if(NOT WIN32)
    set(AGENT_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/install" CACHE PATH
        "MicroXRCEAgent install dir")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/MicroXrceAgent.cmake)

# Printed so the lane can hand the suite the exact path the recipe settled on.
message(STATUS "MICRO_XRCE_AGENT_PATH=${AGENT_BINARY}")
message(STATUS "MICRO_XRCE_AGENT_LIB_DIR=${AGENT_LIB_DIR}")
