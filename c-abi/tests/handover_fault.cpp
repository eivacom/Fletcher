// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// TEST-ONLY. The hand-over fault hook (D9S-6, D-D9S-4; d9 L18, BC-12). Compiled into the test-only
// `fletcher-c-abi-handover-fault` shim alone (tests/CMakeLists.txt), never into a shipped one: the
// shipped shim has no such source, no such macro, and so no such export.
//
// `fl_test_handover_fault_arm(nth)` makes the nth hand-over from now fail, once: the fault point in
// `batches_server.cpp` throws `std::bad_alloc` after the export, which is the one failure a
// hand-over can really meet. `fl_test_handover_fault_fired()` says how many armed faults fired
// since the last arm, so a run in which nothing failed cannot pass for a green one. State is two
// atomics, per process (each gtest TEST runs in its own process under ctest).
#ifndef FLETCHER_C_ABI_HANDOVER_FAULT
#error \
    "handover_fault.cpp belongs to the test-only fletcher-c-abi-handover-fault shim, which defines FLETCHER_C_ABI_HANDOVER_FAULT; it is never compiled into a shipped shim"
#endif

#include <atomic>
#include <cstdint>
#include <new>

#include "fletcher/abi/binding.h"

extern "C" {
FL_ABI_EXPORT void fl_test_handover_fault_arm(int64_t nth);
FL_ABI_EXPORT int64_t fl_test_handover_fault_fired(void);
}

namespace {
std::atomic<int64_t> g_remaining{0};  // hand-overs until the armed one; 0 or less: disarmed
std::atomic<int64_t> g_fired{0};
}  // namespace

namespace fletcher::abi::internal {

/// The fault point `batches_server.cpp` calls inside its hand-over (FLETCHER_HANDOVER_FAULT_POINT).
void HandoverFaultPoint() {
    int64_t left = g_remaining.load();
    while (left > 0 && !g_remaining.compare_exchange_weak(left, left - 1)) {
    }
    if (left == 1) {
        g_fired.fetch_add(1);
        throw std::bad_alloc();
    }
}

}  // namespace fletcher::abi::internal

void fl_test_handover_fault_arm(int64_t nth) {
    g_fired.store(0);
    g_remaining.store(nth);
}

int64_t fl_test_handover_fault_fired(void) { return g_fired.load(); }
