// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The BINDING ABI — the pure C surface that language bindings call Fletcher
// through (round BIND, tracker Part 2).
//
// Position: ABOVE the seam. Fletcher is the CALLEE here; an application, via a
// generated or hand-written binding, is the caller. That is the mirror image of
// the DRIVER ABI (`fletcher/abi/driver.h`, PDA-ABI), which sits BELOW the seam
// with Fletcher as the caller. The two headers are derived from the same seam
// specification and are deliberately NOT shared, NOT included by one another and
// NOT defined in terms of one another (seam §1, PDA-ABI decision 2, D-BIND-2').
// Any resemblance between the two is a consequence of one seam, not a coupling.
//
// Pure C99, self-contained, no dependency on any Fletcher C++ header: P/Invoke
// (and, next round, Rust's `extern "C"`) can see nothing else.
//
// ── Versioning and compatibility ────────────────────────────────────────────
// `fl_binding_abi_version()` returns the ABI version of the shim actually
// loaded, packed as (major << 16) | minor, so a binding compiled against
// FL_BINDING_ABI_VERSION can refuse a shim it does not understand rather than
// crash inside a struct whose layout moved.
//
// Within a major version this header is APPEND-ONLY: existing structs grow only
// at the end, existing functions never change signature, and enumerators keep
// their numbers. Before 1.0 there is NO compatibility guarantee at all (the
// pre-1.0 exemption the rest of the repository already runs under); the
// deprecation policy that replaces it at 1.0 is stated here in BIND-1, which
// specifies this header in full and is reviewed as a specification.
//
// ── BIND-0 status ───────────────────────────────────────────────────────────
// This is the KICKOFF skeleton. Deliberately the whole surface is the version
// function: BIND-0 exists to make the two new lanes (`ci.c-abi.yml`,
// `ci.dotnet.yml`) run on BOTH platforms before any real code exists (seam
// §12.4 — the first lane run of PR #126 found seven defects local green could
// not, three of them Linux-only). Every other declaration arrives in BIND-1.
#ifndef FLETCHER_C_ABI_INCLUDE_FLETCHER_ABI_BINDING_H_
#define FLETCHER_C_ABI_INCLUDE_FLETCHER_ABI_BINDING_H_

#include <stdint.h>

/* Symbol visibility. FLETCHER_C_ABI_BUILDING is defined by the shim's own
 * build only (CMake PRIVATE define), so a consumer gets the import form on
 * Windows and a plain declaration elsewhere. Everything not marked with this
 * macro is hidden: the CMake target sets visibility to hidden on gcc/clang and
 * Windows exports nothing without __declspec(dllexport), so the shim's export
 * table is exactly the functions declared below. */
#if defined(_WIN32)
#if defined(FLETCHER_C_ABI_BUILDING)
#define FL_ABI_EXPORT __declspec(dllexport)
#else
#define FL_ABI_EXPORT __declspec(dllimport)
#endif
#else
#if defined(FLETCHER_C_ABI_BUILDING)
#define FL_ABI_EXPORT __attribute__((visibility("default")))
#else
#define FL_ABI_EXPORT
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The version this header describes. Compare against the loaded shim's
 * fl_binding_abi_version() at load time; `Eiva.Fletcher.Interop` does exactly
 * that in its static constructor. */
#define FL_BINDING_ABI_VERSION_MAJOR 0
#define FL_BINDING_ABI_VERSION_MINOR 1
#define FL_BINDING_ABI_VERSION                                   \
    ((uint32_t)(((uint32_t)FL_BINDING_ABI_VERSION_MAJOR << 16) | \
                (uint32_t)FL_BINDING_ABI_VERSION_MINOR))

/* The ABI version of the loaded shim, packed as (major << 16) | minor.
 *
 * Callable before anything else and from any thread; it touches no state. */
FL_ABI_EXPORT uint32_t fl_binding_abi_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLETCHER_C_ABI_INCLUDE_FLETCHER_ABI_BINDING_H_ */
