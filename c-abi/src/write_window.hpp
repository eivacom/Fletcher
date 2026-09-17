// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The two write-window adapters (BIND-2c).
//
// The seam's `WriteBuffer` and the ABI's `fl_write_window` are the same idea on
// either side of a C boundary — a window plus a refill hook, ONE CROSSING PER
// REFILL rather than one per append — so the adapters are thin by construction.
// They run in opposite directions and it is worth naming which is which:
//
//   WindowBuffer   the CALLER supplies the window, Fletcher writes into it.
//                  Used by `fl_encode_row`, where a binding wants the bytes in
//                  its own buffer. Refills call back out through `grow`.
//
//   CallerWriter   FLETCHER supplies the buffer, the caller writes into it.
//                  Used by `fl_publisher_publish_raw`, where the row's bytes
//                  come to exist inside the transport's own window and are never
//                  copied into it. This is the direction the copy oracle scores.
#ifndef FLETCHER_C_ABI_SRC_WRITE_WINDOW_HPP_
#define FLETCHER_C_ABI_SRC_WRITE_WINDOW_HPP_

#include <cstring>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <stdexcept>
#include <string>

#include "fletcher/abi/binding.h"

namespace fletcher::abi {

/// A `WriteBuffer` over a caller-owned `fl_write_window`.
///
/// The base class does the arithmetic; this subclass only has to answer "the
/// window is full" by asking the caller for more. `origin` is the containment
/// site's attribution slot: a refusal produced by the caller's own `grow` is the
/// CALLER's, not the seam's, and saying so is the difference between "Fletcher
/// failed" and "your buffer would not grow".
class WindowBuffer final : public WriteBuffer {
   public:
    WindowBuffer(fl_write_window* window, fl_origin* origin)
        : WriteBuffer(window->data, window->capacity, window->pos),
          window_(window),
          origin_(origin) {
        // Bounds by SUBTRACTION is the window's own rule, and every subtraction
        // in the base class assumes this. A window handed over with `pos` above
        // `capacity` would underflow all of them into a ~2^64 room and hand the
        // encoder a span over a handful of real bytes.
        if (window->pos > window->capacity) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_write_window: pos (" + std::to_string(window->pos) +
                                  ") is above capacity (" + std::to_string(window->capacity) + ")");
        }
    }

    /// Publish the final cursor back into the caller's window.
    ///
    /// Called only on success. A failed encode leaves `pos` where it was, which
    /// is the honest answer: bytes may have been written above it, and those are
    /// the same disclosed residue the header already documents for a writer that
    /// over-reports.
    void Commit() noexcept {
        window_->data = data_;
        window_->capacity = capacity_;
        window_->pos = pos_;
    }

   protected:
    void AppendSlow(const uint8_t* data, size_t len) override {
        Grow(len);
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendZerosSlow(size_t len) override {
        Grow(len);
        std::memset(data_ + pos_, 0, len);
        pos_ += len;
    }

   private:
    void Grow(size_t min_bytes) {
        if (window_->grow == nullptr) {
            // A fixed-capacity window that ran out. `std::overflow_error` is the
            // NORMATIVE spelling (seam §5.1) and the containment site turns it
            // into FL_PAYLOAD_TOO_LARGE, which is exactly what the header
            // promises for this case.
            throw std::overflow_error(
                "fl_write_window: the row does not fit the caller's fixed-capacity window (" +
                std::to_string(capacity_) + " bytes, " + std::to_string(pos_) + " used, " +
                std::to_string(min_bytes) + " more needed)");
        }

        // The callee reads all three fields, so they are synced down before the
        // call and read back after it — the base class has been keeping the
        // authoritative copy since the last refill.
        window_->data = data_;
        window_->capacity = capacity_;
        window_->pos = pos_;

        // Saved, not assumed. An earlier version restored FL_ORIGIN_SEAM here,
        // which is wrong for `fl_encode_row`: its ambient origin is CODEC, so a
        // row that refilled once and then failed in the codec was reported as
        // the seam's failure. The attribution belongs to whoever set it.
        const fl_origin ambient = *origin_;

        fl_error err = {};
        *origin_ = FL_ORIGIN_CALLBACK;
        const fl_status status = window_->grow(window_, min_bytes, &err);
        if (status != FL_OK) {
            // The caller's message, COPIED and carried through rather than
            // replaced: it is the only thing that says WHY the buffer would not
            // grow, and this frame knows nothing about that.
            //
            // Copied and NOT freed, per D-BIND-32: an `fl_error` a callback fills
            // is BORROWED to the shim, exactly as `fl_str` is everywhere else in
            // the ABI. Freeing it here would hand a binding's allocation to the
            // shim's `delete[]`, and those are one heap only while both sides
            // share a C runtime — which this shim, linking the MSVC CRT
            // statically, does not.
            std::string message(reinterpret_cast<const char*>(err.message), err.message_len);
            throw PubSubError(static_cast<PubSubStatus>(status),
                              message.empty() ? "fl_write_window: grow refused" : message);
        }
        // Only on SUCCESS: a `grow` that threw above keeps CALLBACK, which is
        // the whole point of setting it.
        *origin_ = ambient;

        data_ = window_->data;
        capacity_ = window_->capacity;
        pos_ = window_->pos;

        // A conforming `grow` returns room; one that does not would otherwise
        // hand the caller a short span through the base class's own check, with
        // a message about Fletcher rather than about the window.
        if (capacity_ < pos_ || capacity_ - pos_ < min_bytes) {
            throw PubSubError(PubSubStatus::kInternal,
                              "fl_write_window: grow reported success without delivering " +
                                  std::to_string(min_bytes) + " contiguous bytes");
        }
    }

    fl_write_window* window_;
    fl_origin* origin_;
};

/// Run the caller's `fl_writer_fn` against Fletcher's own buffer, in place.
///
/// The zero-return rule (D-BIND-19 rule 3): a binding's writer thunk catches
/// everything on its own side, records the exception there and returns 0. There
/// is no other way for it to report failure — the signature returns a count, not
/// a status — so 0 is the signal, and this turns it into a failed publish that
/// the binding then answers by rethrowing the original managed exception with
/// its own stack. A producer with genuinely nothing to write must not be here:
/// `min_bytes` is required to be non-zero, so it has already said it needs room.
inline void WriteThroughCaller(WriteBuffer& out, fl_writer_fn writer, void* ctx, size_t min_bytes,
                               fl_origin* origin) {
    out.AppendInPlace(min_bytes, [&](uint8_t* dst, size_t room) -> size_t {
        const fl_origin ambient = *origin;
        *origin = FL_ORIGIN_CALLBACK;
        const size_t written = writer(ctx, dst, room);
        if (written == 0) {
            throw PubSubError(PubSubStatus::kInternal,
                              "fl_publisher_publish_raw: the caller's writer reported 0 bytes, "
                              "which is how a binding signals that its own thunk captured an "
                              "exception; nothing was published");
        }
        *origin = ambient;
        return written;
    });
}

}  // namespace fletcher::abi

#endif  // FLETCHER_C_ABI_SRC_WRITE_WINDOW_HPP_
