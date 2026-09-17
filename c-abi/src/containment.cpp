// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "containment.hpp"

#include <cstring>
#include <fletcher/core/status.hpp>
#include <new>
#include <stdexcept>
#include <string>

#include "single_copy.hpp"

namespace fletcher::abi {
namespace {

/// Copy `what` onto the heap for the caller to own.
///
/// `what()` returns a `const char*`, so whatever arrives here is already a C
/// string and cannot carry an embedded zero byte — `PubSubError` rewrites each
/// one as the four characters `\x00` at construction (core/status.hpp), and
/// every other `std::exception` message is a C string by definition. The
/// header's "may contain no zero byte" promise is therefore inherited rather
/// than re-enforced here.
///
/// `new[]` pairs with the `delete[]` in `fl_error_dispose`, and the two must
/// keep pairing: a message allocated with `malloc` and freed with `delete[]` is
/// the kind of mismatch that survives every test on one platform.
void SetMessage(fl_error* err, const char* what) noexcept {
    if (err == nullptr || what == nullptr) return;
    const size_t len = std::strlen(what);
    if (len == 0) return;

    auto* buffer = new (std::nothrow) uint8_t[len];
    if (buffer == nullptr) return;  // the number is worth more than nothing
    std::memcpy(buffer, what, len);
    err->message = buffer;
    err->message_len = len;
}

}  // namespace

fl_status PoisonedRefusal(fl_error* err) noexcept {
    // One site, again. A shim that found a second copy of itself at load has to
    // refuse every fallible entry point, and the containment wrapper is already
    // the one place all of them pass through - so the refusal is written once
    // rather than forty times, and an entry point cannot forget it by being
    // added later.
    try {
        throw PubSubError(PubSubStatus::kInternal, SingleCopyRefusal());
    } catch (...) {
        return Capture(err, FL_ORIGIN_SEAM);
    }
}

fl_status Capture(fl_error* err, fl_origin origin) noexcept {
    fl_status status = FL_INTERNAL;
    const char* what = "an exception of unknown type crossed the binding ABI";

    try {
        throw;
    } catch (const PubSubError& e) {
        // The seam's own failures arrive numbered. `PubSubError` has already
        // coerced kOk/kPending/kSubscriptionEnded to kInternal, so a failure
        // cannot reach here carrying a non-failure number.
        status = static_cast<fl_status>(e.status());
        what = e.what();
    } catch (const std::overflow_error& e) {
        // NORMATIVE (seam §5.1), by TYPE and not by source: this is the only
        // producer of FL_PAYLOAD_TOO_LARGE in the tree, and without the rule
        // taxonomy entry 4 is unreachable and a row that does not fit the
        // transport's bound degrades to "internal", which tells a caller nothing
        // it can act on.
        status = FL_PAYLOAD_TOO_LARGE;
        what = e.what();
    } catch (const std::invalid_argument& e) {
        // The positional reader's refusal on malformed bytes. Recognised at the
        // CODEC sites only: at a seam site the normative rule above is total and
        // everything unlisted is kInternal, and reproducing the seam exactly
        // matters more there than a finer number (see the header comment).
        status = origin == FL_ORIGIN_CODEC ? FL_INVALID_ARGUMENT : FL_INTERNAL;
        what = e.what();
    } catch (const std::out_of_range& e) {
        // `EncodeRow`'s row-index check and `WriteBuffer::PatchByte`'s offset
        // check. Same reasoning as above.
        status = origin == FL_ORIGIN_CODEC ? FL_INVALID_ARGUMENT : FL_INTERNAL;
        what = e.what();
    } catch (const std::exception& e) {
        status = FL_INTERNAL;
        what = e.what();
    } catch (...) {
        // A non-`std::exception` type loses its identity entirely — "the price
        // of a boundary that cannot let an untyped exception through" (spec
        // §5.1). The default message above is what the caller gets.
    }

    if (err != nullptr) {
        err->status = static_cast<int32_t>(status);
        err->origin = static_cast<int32_t>(origin);
        err->message = nullptr;
        err->message_len = 0;
        SetMessage(err, what);
    }
    return status;
}

}  // namespace fletcher::abi

extern "C" {

void fl_error_dispose(fl_error* err) {
    if (err == nullptr) return;
    delete[] err->message;
    err->status = static_cast<int32_t>(FL_OK);
    err->origin = static_cast<int32_t>(FL_ORIGIN_NONE);
    err->message = nullptr;
    err->message_len = 0;
}

}  // extern "C"
