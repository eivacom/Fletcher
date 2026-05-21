# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-pubsub-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::pubsub  ->  fletcher-pubsub::fletcher-pubsub
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::pubsub)
#   target_link_libraries(my_target PRIVATE fletcher-pubsub::fletcher-pubsub)

if(TARGET fletcher-pubsub::fletcher-pubsub AND NOT TARGET fletcher::pubsub)
    add_library(fletcher::pubsub ALIAS fletcher-pubsub::fletcher-pubsub)
endif()
