/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 *
 * A DECOY second shim, for one test and nothing else.
 *
 * The single-copy check (D-BIND-17) is only worth having if it actually fires,
 * and the only way to prove that is to put a second module exporting the marker
 * into a live process. This is that module: it exports the marker symbol and
 * nothing else, it is never packaged, and it is never loaded except by
 * `SingleCopy.ASecondMarkerExportIsFound`.
 *
 * Deliberately C, and deliberately not linked against anything: it has to be a
 * module that LOOKS like a second shim to `GetProcAddress`/`dlsym` without being
 * one, because building a real second shim would take the whole eProsima chain
 * with it.
 */
#if defined(_WIN32)
#define DECOY_EXPORT __declspec(dllexport)
#else
#define DECOY_EXPORT __attribute__((visibility("default")))
#endif

DECOY_EXPORT const char* fl_single_copy_marker(void) {
    return "decoy second shim, for the single-copy test only";
}
