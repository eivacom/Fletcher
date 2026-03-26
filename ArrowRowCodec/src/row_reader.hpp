#ifndef ARROW_ROW_SRC_ROW_READER_HPP_
#define ARROW_ROW_SRC_ROW_READER_HPP_

// Internal implementation detail shared between row_codec.cpp and
// row_batcher.cpp.  Not part of the public API.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace arrow_row {
namespace detail {

struct Reader {
    const uint8_t* data;
    size_t         size;
    size_t         pos{0};

    template <typename T>
    T Read() {
        if (pos + sizeof(T) > size)
            throw std::invalid_argument("arrow_row: buffer underrun");
        T value;
        std::memcpy(&value, data + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    const uint8_t* ReadBytes(size_t n) {
        if (pos + n > size)
            throw std::invalid_argument("arrow_row: buffer underrun");
        const uint8_t* ptr = data + pos;
        pos += n;
        return ptr;
    }
};

}  // namespace detail
}  // namespace arrow_row

#endif  // ARROW_ROW_SRC_ROW_READER_HPP_
