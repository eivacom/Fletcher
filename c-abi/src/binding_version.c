// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The one entry point the BIND-0 skeleton exports.
//
// Deliberately a .c file, not a .cpp: it is the standing proof that the header
// is consumable as C99 by the shim's own build and not only by the C++ tests.
#include "fletcher/abi/binding.h"

uint32_t fl_binding_abi_version(void) { return FL_BINDING_ABI_VERSION; }
