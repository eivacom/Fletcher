#include "row_batcher.hpp"

namespace arrow_row {

RowBatcher::RowBatcher(std::shared_ptr<arrow::Schema> schema)
    : schema_(std::move(schema)) {}

}  // namespace arrow_row
