/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Copyright (C) 2026 The Fletcher Authors
 *
 * The PROBE SHIM's test surface (D-BIND-58, D-BIND-61, D-BIND-62).
 *
 * TEST-ONLY. Exported by `fletcher-c-abi-probe` alone - the shim built with
 * `SeamProbeProvider` registered as the builtin `probe` - and NEVER by the
 * shipped `fletcher-c-abi`, whose surface is `binding.h` and nothing else. It is
 * a header of its own for that reason: nothing a binding compiles against in
 * production can see it.
 *
 * What it is for. The copy oracle scores a publish by address provenance, and
 * the probe is the only provider whose window it can see into. A binding selects
 * `probe` by name, publishes through its REAL publish path - the one that ships -
 * and asks these two functions what the probe saw. The scoring is the copy
 * oracle's own `Judge()` (`fletcher-copy-probe`), so a binding leg and the C++
 * legs cannot score differently.
 *
 * Everything here is BORROWED for the call, like every `binding.h` entry point,
 * and fails through `fl_error` like them.
 */
#ifndef FLETCHER_ABI_TEST_PROBE_H_
#define FLETCHER_ABI_TEST_PROBE_H_

#include "fletcher/abi/binding.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the binding's subscriber saw of the publish being scored, sampled INSIDE
 * its delivery callback, while the pointers were still borrowed. `loaned` is the
 * delivered value of the attachment `fl_test_probe_loan` parked, or null if the
 * delivery did not carry it - which scores as a copy, never as a pass. */
typedef struct fl_test_delivery {
    const uint8_t* row;
    size_t row_len;
    const uint8_t* loaned;
    size_t loaned_len;
} fl_test_delivery;

/* `Judge()`'s verdict, plus the addresses it was decided on, so a failing leg can
 * say WHERE the bytes came from rather than only that they moved.
 *
 * `encode_copies` is -1 when the producer half was never sampled - it never
 * defaults to a number (the ledger's rule). `produced_at` is where the window's
 * payload came from; `encode_base`/`encode_len` are the window itself. */
typedef struct fl_test_verdict {
    int64_t encode_copies;
    uint64_t row_copies;
    uint64_t attachment_copies;
    uintptr_t produced_at;
    uintptr_t encode_base;
    uint64_t encode_len;
} fl_test_verdict;

/* Park `bytes` in the probe's own memory - where a transport's loaned sample
 * would be - and make every later publish on `provider` deliver them as the
 * attachment `key`. `*base` is where they were parked: the address a delivery
 * that did not copy them must carry. FL_INVALID_ARGUMENT unless `provider` was
 * created from the selector `probe`. */
FL_ABI_EXPORT fl_status fl_test_probe_loan(fl_provider* provider, fl_str key, const uint8_t* bytes,
                                           size_t len, uintptr_t* base, fl_error* err);

/* Score the LAST publish on `provider` (D-BIND-61).
 *
 * `payload`/`payload_len` are the bytes of the row's one variable-length field in
 * the CALLER'S OWN buffer, still live: `encode_copies` is 0 only if the window's
 * copy of them was appended from exactly there. `delivery` is what the caller's
 * subscriber saw. FL_INVALID_ARGUMENT, with the reason, when the publish cannot
 * be scored - the provider is not a probe, the trace faulted, or the payload is
 * not in the row. */
FL_ABI_EXPORT fl_status fl_test_probe_score(fl_provider* provider, const uint8_t* payload,
                                            size_t payload_len, const fl_test_delivery* delivery,
                                            fl_test_verdict* out, fl_error* err);

#ifdef __cplusplus
}
#endif

#endif /* FLETCHER_ABI_TEST_PROBE_H_ */
