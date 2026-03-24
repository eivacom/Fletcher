#ifndef ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_
#define ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_

#include "write_ahead_log.hpp"

#include <arrow/api.h>

#include <memory>

namespace arrow_row {

class RowBatcher {
 public:
    // schema must match the schema used by EncodeRow().
    // wal is the write-ahead log used to buffer and retrieve rows.
    RowBatcher(std::shared_ptr<arrow::Schema> schema, WriteAheadLog& wal);

    // Encode one row into the write-ahead log.
    // Throws std::invalid_argument on a malformed or schema-mismatched buffer.
    void Append(const ArrowRow& buf);

    // Read all buffered rows from the log as an Arrow Table, reset the log,
    // and return the table.  The batcher is ready to accept new rows immediately.
    // Returns a zero-row table if called before any Append().
    std::shared_ptr<arrow::Table> Flush();

    // Rows accumulated since construction or the last Flush().
    int64_t row_count() const noexcept { return row_count_; }

 private:
    std::shared_ptr<arrow::Schema> schema_;
    WriteAheadLog&                 wal_;
    std::unique_ptr<LogHandle>     handle_;
    int64_t                        row_count_{0};
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_ROW_BATCHER_HPP_
