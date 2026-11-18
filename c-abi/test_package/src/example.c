/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 */
#include <stdio.h>
#include <string.h>

#include "fletcher/abi/binding.h"

/* argv[1] is the build the PACKAGE records (`edge` or `server`, its `variant`
 * option, passed by test_package/conanfile.py). The shim's own answer must name
 * exactly that build. Without an expectation this program fails: a check that
 * accepted either build would pass a server package that held the edge shim, or
 * the reverse, and this is the one place the package's identity meets the shim's
 * own answer from outside. */
int main(int argc, char** argv) {
    uint32_t version = fl_binding_abi_version();
    printf("fletcher binding ABI %u.%u\n", version >> 16, version & 0xFFFFu);
    fl_variant variant = fl_binding_variant();
    const char* name = variant == FL_VARIANT_EDGE     ? "edge"
                       : variant == FL_VARIANT_SERVER ? "server"
                                                      : "unknown";
    printf("fletcher shim build %s\n", name);
    if (argc != 2 || (strcmp(argv[1], "edge") != 0 && strcmp(argv[1], "server") != 0)) {
        fprintf(stderr, "usage: example <edge|server>, the build the package records\n");
        return 1;
    }
    if (strcmp(name, argv[1]) != 0) {
        fprintf(stderr, "the package records the %s build, and its shim answers %s\n", argv[1], name);
        return 1;
    }
    return version == FL_BINDING_ABI_VERSION ? 0 : 1;
}
