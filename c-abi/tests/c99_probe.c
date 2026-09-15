/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 *
 * The self-containment probe for `fletcher/abi/binding.h`.
 *
 * This translation unit includes the binding header FIRST and includes nothing
 * else, then uses every declaration it publishes. It is compiled as strict C
 * with warnings promoted to errors, which is what makes two claims testable
 * rather than asserted:
 *
 *   * the header is pure C — no C++ keyword, no `//` -only construct that a C99
 *     compiler rejects, no `bool`/`nullptr` smuggled in from a C++ header;
 *   * the header is SELF-CONTAINED — it pulls in whatever it needs (`stdint.h`
 *     for the fixed-width return type) rather than assuming the consumer
 *     included it first.
 *
 * Both matter because the consumers are P/Invoke and, next round, Rust's
 * `extern "C"`: neither can read a C++ header, and neither has a "the other
 * header was included first" to fall back on.
 */
#include "fletcher/abi/binding.h"

/* No link step: the probe is compiled as an object library only, so a
 * declaration is all that is exercised here. The RUNTIME value is checked by
 * BindingAbi.VersionMatchesHeader, which links the real shim. */
uint32_t fletcher_c99_probe_version_constant(void);

uint32_t fletcher_c99_probe_version_constant(void) {
    uint32_t declared = FL_BINDING_ABI_VERSION;
    uint32_t major = (uint32_t)FL_BINDING_ABI_VERSION_MAJOR;
    uint32_t minor = (uint32_t)FL_BINDING_ABI_VERSION_MINOR;
    return declared ^ ((major << 16) | minor); /* 0 when the packing agrees */
}
