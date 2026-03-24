#ifndef ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_
#define ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_

#include "row_codec.hpp"

#include <arrow/api.h>

#include <atomic>
#include <functional>
#include <memory>

namespace arrow_row {

class RowBatcher {
 public:
    using FlushCallback = std::function<void(std::shared_ptr<arrow::Table>)>;

    RowBatcher(std::shared_ptr<arrow::Schema> schema,
               int64_t                        batch_size,
               FlushCallback                  on_flush);
    virtual ~RowBatcher() = default;

    // Appends one encoded row. When the row count reaches batch_size, the
    // flush callback is invoked with the accumulated Arrow Table and the
    // counter is reset to zero.
    // Throws std::invalid_argument on a malformed or schema-mismatched buffer.
    void Append(const ArrowRow& buf);

    // Rows accumulated since construction or the last automatic flush.
    int64_t row_count() const noexcept { return row_count_; }

 protected:
    virtual void DoAppend(const ArrowRow& buf) = 0;
    virtual std::shared_ptr<arrow::Table> DoFlush() = 0;

    std::shared_ptr<arrow::Schema> schema_;

 private:
    int64_t       batch_size_;
    FlushCallback on_flush_;
    std::atomic<int64_t> row_count_{0};
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_
