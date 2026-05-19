# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-pubsub-arrow-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::pubsub-arrow  ->  eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow

if(TARGET eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow AND NOT TARGET fletcher::pubsub-arrow)
    add_library(fletcher::pubsub-arrow ALIAS eiva-fletcher-pubsub-arrow::eiva-fletcher-pubsub-arrow)
endif()
