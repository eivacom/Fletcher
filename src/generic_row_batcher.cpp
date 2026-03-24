#include "generic_row_batcher.hpp"

namespace arrow_row {

GenericRowBatcher::GenericRowBatcher(std::shared_ptr<arrow::Schema> schema, WriteAheadLog& wal)
    : RowBatcher(std::move(schema)), wal_(wal), handle_(wal_.CreateLog(*schema_, {})) {}

void GenericRowBatcher::Append(const ArrowRow& buf) {
    handle_->Log(buf);
    ++row_count_;
}

std::shared_ptr<arrow::Table> GenericRowBatcher::Flush() {
    auto table = handle_->ToTable();
    handle_    = wal_.CreateLog(*schema_, {});
    row_count_ = 0;
    return table;
}

}  // namespace arrow_row
