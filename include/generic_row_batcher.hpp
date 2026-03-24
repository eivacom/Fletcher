#ifndef ARROW_ROW_INCLUDE_GENERIC_ROW_BATCHER_HPP_
#define ARROW_ROW_INCLUDE_GENERIC_ROW_BATCHER_HPP_

#include "row_batcher.hpp"
#include "write_ahead_log.hpp"

#include <memory>

namespace arrow_row {

// Concrete RowBatcher that buffers rows through a WriteAheadLog and returns
// them as an Arrow Table on Flush().
class GenericRowBatcher : public RowBatcher {
 public:
    GenericRowBatcher(std::shared_ptr<arrow::Schema> schema, WriteAheadLog& wal);

    void Append(const ArrowRow& buf) override;
    std::shared_ptr<arrow::Table> Flush() override;
    int64_t row_count() const noexcept override { return row_count_; }

 private:
    WriteAheadLog&             wal_;
    std::unique_ptr<LogHandle> handle_;
    int64_t                    row_count_{0};
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_GENERIC_ROW_BATCHER_HPP_
