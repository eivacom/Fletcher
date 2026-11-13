// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The one entry point the BIND-0 skeleton exports.
//
// Deliberately a .c file, not a .cpp: it is the standing proof that the header
// is consumable as C99 by the shim's own build and not only by the C++ tests.
#include "fletcher/abi/binding.h"

uint32_t fl_binding_abi_version(void) { return FL_BINDING_ABI_VERSION; }

// The single place that decides which build this is (register BC-10). D9S-2
// turns the literal into a per-target constant set in c-abi/CMakeLists.txt; the
// edge build's refusals of the batched entry points ask this function, and
// decide nothing themselves. Plain C, outside the containment wrapper.
fl_variant fl_binding_variant(void) { return FL_VARIANT_EDGE; }
