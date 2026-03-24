#pragma once

#include "write_ahead_log.h"

#include <arrow/api.h>

#include <memory>

namespace arrow_row {

class RowBatcher {
public:
    /// @param schema  Must match the schema used by encodeRow().
    /// @param wal     Write-ahead log used to buffer and retrieve rows.
    RowBatcher(std::shared_ptr<arrow::Schema> schema, WriteAheadLog& wal);

    /// Encode one row into the write-ahead log.
    /// Throws std::invalid_argument on a malformed or schema-mismatched buffer.
    void append(const ArrowRow& buf);

    /// Read all buffered rows from the log as an Arrow Table, reset the log,
    /// and return the table.  The batcher is ready to accept new rows immediately.
    /// Returns a zero-row table if called before any append().
    std::shared_ptr<arrow::Table> flush();

    /// Rows accumulated since construction or the last flush().
    int64_t rowCount() const noexcept { return rowCount_; }

private:
    std::shared_ptr<arrow::Schema> schema_;
    WriteAheadLog&                 wal_;
    std::unique_ptr<LogHandle>     handle_;
    int64_t                        rowCount_{0};
};

} // namespace arrow_row
