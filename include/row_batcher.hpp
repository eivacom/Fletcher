#ifndef ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_
#define ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_

#include "row_codec.hpp"

#include <arrow/api.h>

#include <memory>

namespace arrow_row {

class RowBatcher {
 public:
    explicit RowBatcher(std::shared_ptr<arrow::Schema> schema);
    virtual ~RowBatcher() = default;

    // Encode one row into the batcher.
    // Throws std::invalid_argument on a malformed or schema-mismatched buffer.
    virtual void Append(const ArrowRow& buf) = 0;

    // Materialise all buffered rows as an Arrow Table and reset the batcher.
    // Returns a zero-row table if called before any Append().
    virtual std::shared_ptr<arrow::Table> Flush() = 0;

    // Rows accumulated since construction or the last Flush().
    virtual int64_t row_count() const noexcept {
        return row_count_;
    };

 protected:
    std::shared_ptr<arrow::Schema> schema_;
    int64_t                        row_count_{ 0 };
    };

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_
