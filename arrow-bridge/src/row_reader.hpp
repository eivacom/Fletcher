// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_SRC_ROW_READER_HPP_
#define FLETCHER_SRC_ROW_READER_HPP_

// Internal implementation detail used by codec.cpp.  Not part of the public API.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace fletcher {
namespace detail {

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos{0};

    // Bytes not yet consumed. Invariant: pos <= size, so this never underflows.
    size_t remaining() const { return size - pos; }

    template <typename T>
    T Read() {
        // Overflow-safe form of `pos + sizeof(T) > size` (pos <= size always).
        if (sizeof(T) > size - pos) throw std::invalid_argument("fletcher: buffer underrun");
        T value;
        std::memcpy(&value, data + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    const uint8_t* ReadBytes(size_t n) {
        // Overflow-safe: a wrapping `pos + n` could otherwise pass the check
        // for an attacker-controlled length and allow an out-of-bounds read.
        if (n > size - pos) throw std::invalid_argument("fletcher: buffer underrun");
        const uint8_t* ptr = data + pos;
        pos += n;
        return ptr;
    }
};

}  // namespace detail
}  // namespace fletcher

#endif  // FLETCHER_SRC_ROW_READER_HPP_
