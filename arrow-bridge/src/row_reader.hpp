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
    size_t         size;
    size_t         pos{0};

    template <typename T>
    T Read() {
        if (pos + sizeof(T) > size)
            throw std::invalid_argument("fletcher: buffer underrun");
        T value;
        std::memcpy(&value, data + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    const uint8_t* ReadBytes(size_t n) {
        if (pos + n > size)
            throw std::invalid_argument("fletcher: buffer underrun");
        const uint8_t* ptr = data + pos;
        pos += n;
        return ptr;
    }
};

}  // namespace detail
}  // namespace fletcher

#endif  // FLETCHER_SRC_ROW_READER_HPP_
