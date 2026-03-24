#include "row_batcher.h"

#include <stdexcept>

namespace arrow_row {

RowBatcher::RowBatcher(std::shared_ptr<arrow::Schema> schema, WriteAheadLog& wal)
    : schema_(std::move(schema)), wal_(wal), handle_(wal_.createLog(*schema_, {})) {}

void RowBatcher::append(const ArrowRow& buf) {
    handle_->log(buf);
    ++rowCount_;
}

std::shared_ptr<arrow::Table> RowBatcher::flush() {
    auto table = handle_->toTable();
    handle_    = wal_.createLog(*schema_, {});
    rowCount_  = 0;
    return table;
}

} // namespace arrow_row
