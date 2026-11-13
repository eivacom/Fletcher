/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 */
#include <stdio.h>

#include "fletcher/abi/binding.h"

int main(void) {
    uint32_t version = fl_binding_abi_version();
    printf("fletcher binding ABI %u.%u\n", version >> 16, version & 0xFFFFu);
    fl_variant variant = fl_binding_variant();
    const char* name = variant == FL_VARIANT_EDGE     ? "edge"
                       : variant == FL_VARIANT_SERVER ? "server"
                                                      : "unknown";
    printf("fletcher shim build %s\n", name);
    if (variant != FL_VARIANT_EDGE && variant != FL_VARIANT_SERVER) return 1;
    return version == FL_BINDING_ABI_VERSION ? 0 : 1;
}
