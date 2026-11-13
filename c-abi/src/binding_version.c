// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The one entry point the BIND-0 skeleton exports.
//
// Deliberately a .c file, not a .cpp: it is the standing proof that the header
// is consumable as C99 by the shim's own build and not only by the C++ tests.
#include "fletcher/abi/binding.h"

uint32_t fl_binding_abi_version(void) { return FL_BINDING_ABI_VERSION; }

// The single place that decides which build this is (register BC-10). It answers
// with the constant c-abi/CMakeLists.txt defines PER TARGET: edge for the shipped
// shim in an edge package and for the test-only probe shim, server for the shipped
// shim in a server package. D9S-4's refusals of the batched entry points on the
// edge build ask this function and decide nothing themselves. An artefact that
// does not state its build does not compile. Plain C, outside the containment
// wrapper.
#ifndef FLETCHER_C_ABI_VARIANT
#error "FLETCHER_C_ABI_VARIANT is not defined: every target compiling this file must say which build it is (FL_VARIANT_EDGE or FL_VARIANT_SERVER), see c-abi/CMakeLists.txt"
#endif

fl_variant fl_binding_variant(void) { return FLETCHER_C_ABI_VARIANT; }
