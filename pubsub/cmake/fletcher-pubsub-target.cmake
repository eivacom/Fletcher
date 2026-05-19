# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-pubsub-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::pubsub  ->  eiva-fletcher-pubsub::eiva-fletcher-pubsub
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::pubsub)
#   target_link_libraries(my_target PRIVATE eiva-fletcher-pubsub::eiva-fletcher-pubsub)

if(TARGET eiva-fletcher-pubsub::eiva-fletcher-pubsub AND NOT TARGET fletcher::pubsub)
    add_library(fletcher::pubsub ALIAS eiva-fletcher-pubsub::eiva-fletcher-pubsub)
endif()
