# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-c-abi-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::c-abi  ->  fletcher-c-abi::fletcher-c-abi

if(TARGET fletcher-c-abi::fletcher-c-abi AND NOT TARGET fletcher::c-abi)
    add_library(fletcher::c-abi ALIAS fletcher-c-abi::fletcher-c-abi)
endif()
