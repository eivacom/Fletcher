/* SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 *
 * The allocation counter both 4d-v harnesses read (D-BIND-48). Linux only.
 *
 * ── Why malloc, and why a preload ──────────────────────────────────────────
 * The shim links the CRT dynamically (D-BIND-38), so an `operator new` replaced
 * in a benchmark EXECUTABLE sees only that executable's allocations on Windows.
 * On Linux it would see the shim's too - but not nanoarrow's, which calls
 * `malloc` directly, and not at all from a .NET process, which has no
 * `operator new` to replace. Interposing `malloc` itself, by LD_PRELOAD, catches
 * every one of those: libstdc++'s `operator new`, nanoarrow, Apache.Arrow's
 * `NativeMemory.Alloc` for its C Data Interface structs, and the runtime's own
 * interop stubs. ONE instrument for both harnesses is the point - two counters
 * with different reach would make the C++ and C# columns incomparable.
 *
 * ── What it counts ─────────────────────────────────────────────────────────
 * ALLOCATION EVENTS on the CALLING THREAD: malloc, calloc, realloc and the
 * aligned forms. `free` is not counted. Thread-local, so a benchmark samples
 * `fbench_alloc_count()` before and after the calls it measures and the
 * runtime's background threads (the GC, the tiered JIT) do not leak into it.
 *
 * `initial-exec` TLS on purpose: the general-dynamic model can call
 * `__tls_get_addr`, which may itself allocate on first touch - inside `malloc`,
 * that is unbounded recursion. A preloaded object gets static TLS, which is
 * what initial-exec requires.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>

extern void* __libc_malloc(size_t size);
extern void* __libc_calloc(size_t count, size_t size);
extern void* __libc_realloc(void* ptr, size_t size);
extern void* __libc_memalign(size_t alignment, size_t size);

static __thread uint64_t g_count __attribute__((tls_model("initial-exec")));

/* Read by the harnesses: dlsym(RTLD_DEFAULT, ...) from C++, [DllImport] from C#. */
uint64_t fbench_alloc_count(void) { return g_count; }

void* malloc(size_t size) {
    ++g_count;
    return __libc_malloc(size);
}

void* calloc(size_t count, size_t size) {
    ++g_count;
    return __libc_calloc(count, size);
}

void* realloc(void* ptr, size_t size) {
    ++g_count;
    return __libc_realloc(ptr, size);
}

void* memalign(size_t alignment, size_t size) {
    ++g_count;
    return __libc_memalign(alignment, size);
}

void* aligned_alloc(size_t alignment, size_t size) {
    ++g_count;
    return __libc_memalign(alignment, size);
}

int posix_memalign(void** out, size_t alignment, size_t size) {
    ++g_count;
    void* p = __libc_memalign(alignment, size);
    if (p == NULL) return 12; /* ENOMEM, without pulling in errno.h's TLS */
    *out = p;
    return 0;
}
