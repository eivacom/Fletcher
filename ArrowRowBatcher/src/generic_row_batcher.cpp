#include "generic_row_batcher.hpp"

namespace arrow_row {

GenericRowBatcher::GenericRowBatcher(std::shared_ptr<arrow::Schema> schema,
                                     WriteAheadLog&                 wal,
                                     int64_t                        batch_size,
                                     FlushCallback                  on_flush)
    : RowBatcher(std::move(schema), batch_size, std::move(on_flush))
    , wal_(wal)
    , handle_(wal_.CreateLog(*schema_, {})) {}

void GenericRowBatcher::DoAppend(const EncodedRow& buf) {
    handle_->Log(buf);
}

std::shared_ptr<arrow::Table> GenericRowBatcher::DoFlush() {
    auto table = handle_->ToTable();
    handle_    = wal_.CreateLog(*schema_, {});
    return table;
}

}  // namespace arrow_row
