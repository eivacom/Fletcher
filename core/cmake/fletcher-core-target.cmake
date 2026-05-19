# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-core-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::core  ->  eiva-fletcher-core::eiva-fletcher-core
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::core)
#   target_link_libraries(my_target PRIVATE eiva-fletcher-core::eiva-fletcher-core)

if(TARGET eiva-fletcher-core::eiva-fletcher-core AND NOT TARGET fletcher::core)
    add_library(fletcher::core ALIAS eiva-fletcher-core::eiva-fletcher-core)
endif()
