/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 */
#include <stdio.h>

#include "fletcher/abi/binding.h"

int main(void) {
    uint32_t version = fl_binding_abi_version();
    printf("fletcher binding ABI %u.%u\n", version >> 16, version & 0xFFFFu);
    return version == FL_BINDING_ABI_VERSION ? 0 : 1;
}
