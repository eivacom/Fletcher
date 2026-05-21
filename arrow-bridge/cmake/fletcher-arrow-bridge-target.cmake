# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-arrow-bridge-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::arrow-bridge  ->  fletcher-arrow-bridge::fletcher-arrow-bridge
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::arrow-bridge)
#   target_link_libraries(my_target PRIVATE fletcher-arrow-bridge::fletcher-arrow-bridge)

if(TARGET fletcher-arrow-bridge::fletcher-arrow-bridge AND NOT TARGET fletcher::arrow-bridge)
    add_library(fletcher::arrow-bridge ALIAS fletcher-arrow-bridge::fletcher-arrow-bridge)
endif()
