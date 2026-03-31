#ifndef FLETCHER_INCLUDE_ROW_BATCHER_HPP_
#define FLETCHER_INCLUDE_ROW_BATCHER_HPP_

#include "row_codec.hpp"

#include <arrow/api.h>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace fletcher {

class RowBatcher {
 public:
    // Callback invoked in a background thread each time a full batch is ready.
    // Return true to signal success, false to signal failure.
    using FlushCallback = std::function<bool(std::shared_ptr<arrow::Table>)>;

    RowBatcher(std::shared_ptr<arrow::Schema> schema,
               int64_t                        batch_size,
               FlushCallback                  on_flush);

    // Waits for all in-flight flush threads to complete before destroying.
    virtual ~RowBatcher();

    // Thread-safe. Appends one encoded row. When the accumulated row count
    // reaches batch_size the flush callback is dispatched on a new thread and
    // the counter is reset; Append returns without waiting for the callback.
    // Throws std::invalid_argument on a malformed or schema-mismatched buffer.
    void Append(const EncodedRow& buf);

    // Rows accumulated since construction or the last automatic flush.
    int64_t row_count() const noexcept { return row_count_; }

 protected:
    virtual void DoAppend(const EncodedRow& buf) = 0;
    virtual std::shared_ptr<arrow::Table> DoFlush() = 0;

    // Called on the flush thread after the callback returns true.
    virtual void OnBatchFlushSucceeded() {}

    // Called on the flush thread after the callback returns false.
    virtual void OnBatchFlushFailed() {}

    std::shared_ptr<arrow::Schema> schema_;

 private:
    void DispatchFlush(std::shared_ptr<arrow::Table> table);

    int64_t       batch_size_;
    FlushCallback on_flush_;

    std::mutex           append_mutex_;   // guards DoAppend / row_count_ / DoFlush
    std::atomic<int64_t> row_count_{0};

    std::mutex                     futures_mutex_;  // guards pending_flushes_
    std::vector<std::future<void>> pending_flushes_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_ROW_BATCHER_HPP_
