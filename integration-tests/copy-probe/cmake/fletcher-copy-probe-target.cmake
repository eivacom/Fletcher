# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# fletcher-copy-probe-target.cmake
#
# Injected into every consumer's build by Conan (cmake_build_modules).
#
# Creates a convenience alias:
#   fletcher::copy_probe  ->  fletcher-copy-probe::fletcher-copy-probe

if(TARGET fletcher-copy-probe::fletcher-copy-probe AND NOT TARGET fletcher::copy_probe)
    add_library(fletcher::copy_probe ALIAS fletcher-copy-probe::fletcher-copy-probe)
endif()
