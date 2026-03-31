#ifndef FLETCHER_INCLUDE_GENERIC_ROW_BATCHER_HPP_
#define FLETCHER_INCLUDE_GENERIC_ROW_BATCHER_HPP_

#include "row_batcher.hpp"
#include "write_ahead_log.hpp"

#include <memory>

namespace fletcher {

// Concrete RowBatcher that buffers rows through a WriteAheadLog and delivers
// them as an Arrow Table to the flush callback once batch_size rows accumulate.
class GenericRowBatcher : public RowBatcher {
 public:
    GenericRowBatcher(std::shared_ptr<arrow::Schema> schema,
                      WriteAheadLog&                 wal,
                      int64_t                        batch_size,
                      FlushCallback                  on_flush);

 protected:
    void DoAppend(const EncodedRow& buf) override;
    std::shared_ptr<arrow::Table> DoFlush() override;

 private:
    WriteAheadLog&             wal_;
    std::unique_ptr<LogHandle> handle_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_GENERIC_ROW_BATCHER_HPP_
