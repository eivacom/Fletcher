#include "row_batcher.hpp"

namespace arrow_row {

RowBatcher::RowBatcher(std::shared_ptr<arrow::Schema> schema,
                       int64_t                        batch_size,
                       FlushCallback                  on_flush)
    : schema_(std::move(schema))
    , batch_size_(batch_size)
    , on_flush_(std::move(on_flush)) {}

void RowBatcher::Append(const ArrowRow& buf) {
    DoAppend(buf);
    if (++row_count_ >= batch_size_) {
        on_flush_(DoFlush());
        row_count_ = 0;
    }
}

}  // namespace arrow_row
