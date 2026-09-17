// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C++ → C containment site (BIND-2c, D-BIND-19 rule 1).
//
// ── Why there is exactly one of these ───────────────────────────────────────
// A C++ exception unwinding through a C function is undefined behaviour, so
// every entry point in `binding.h` has to be a `catch (...)` boundary. Written
// per entry point that is forty boundaries, forty chances for one to catch a
// narrower set than its neighbour, and no way to read the taxonomy off the code.
// Written once it is one function whose table IS the taxonomy, and every entry
// point is a one-line call that cannot get it wrong by forgetting.
//
// ── Two tables, not one, and why that is not a loophole ─────────────────────
// The seam's mapping is normative and short (spec §5.1): a `PubSubError` carries
// its own status, a `std::overflow_error` becomes `kPayloadTooLarge`, and
// EVERYTHING ELSE becomes `kInternal` carrying the original `what()`. A C driver
// author must reproduce it exactly or drift, so the seam-facing entry points
// here reproduce it exactly.
//
// The CODEC's entry points are not seam entry points. They are BIND's own, and
// `binding.h` already says what they must report: the `fl_origin` documentation
// contrasts *"an FL_INVALID_ARGUMENT from the seam"* with *"an
// FL_INVALID_ARGUMENT from the positional reader on a truncated buffer"*, which
// is only reachable if the reader's `std::invalid_argument` arrives as
// FL_INVALID_ARGUMENT rather than as FL_INTERNAL. So the codec sites recognise
// the reader's two refusal types and the seam sites do not. The origin selects
// the table, which is also the field a binding switches on to raise its own
// typed format exception (D-BIND-15).
#ifndef FLETCHER_C_ABI_SRC_CONTAINMENT_HPP_
#define FLETCHER_C_ABI_SRC_CONTAINMENT_HPP_

#include <utility>

#include "fletcher/abi/binding.h"

namespace fletcher::abi {

/// Fill `err` from the exception currently being handled and return its status.
///
/// Callable only from inside a `catch` block; it rethrows to classify. `err` may
/// be NULL, in which case the status is still returned and the message dropped —
/// a caller that did not ask for the message still gets the number.
///
/// Never throws. If the message cannot be allocated, the number survives and the
/// message is left NULL: losing the diagnostic is bad, terminating in a
/// `noexcept` unwind is worse.
fl_status Capture(fl_error* err, fl_origin origin) noexcept;

/// THE containment site. `origin` is read at CATCH time, so a nested thunk can
/// re-attribute the failure before it propagates — which is how a codec error
/// thrown inside a fused publish arrives as FL_ORIGIN_CODEC even though the
/// entry point is a seam one.
template <typename Fn>
fl_status Contain(fl_error* err, const fl_origin* origin, Fn&& fn) noexcept {
    try {
        std::forward<Fn>(fn)();
        return FL_OK;
    } catch (...) {
        return Capture(err, *origin);
    }
}

/// The fixed-origin form, for an entry point with only one kind of failure.
template <typename Fn>
fl_status Contain(fl_error* err, fl_origin origin, Fn&& fn) noexcept {
    return Contain(err, &origin, std::forward<Fn>(fn));
}

}  // namespace fletcher::abi

#endif  // FLETCHER_C_ABI_SRC_CONTAINMENT_HPP_
