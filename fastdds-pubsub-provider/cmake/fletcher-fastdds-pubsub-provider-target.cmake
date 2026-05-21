# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-fastdds-pubsub-provider-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::fastdds-pubsub-provider  ->  fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
#
# Consumers can use either name:
#   target_link_libraries(my_target PRIVATE fletcher::fastdds-pubsub-provider)
#   target_link_libraries(my_target PRIVATE fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)

if(TARGET fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider AND NOT TARGET fletcher::fastdds-pubsub-provider)
    add_library(fletcher::fastdds-pubsub-provider ALIAS fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)
endif()
