#include "row_batcher.hpp"

#include <algorithm>
#include <chrono>

namespace arrow_row {

RowBatcher::RowBatcher(std::shared_ptr<arrow::Schema> schema,
                       int64_t                        batch_size,
                       FlushCallback                  on_flush)
    : schema_(std::move(schema))
    , batch_size_(batch_size)
    , on_flush_(std::move(on_flush)) {}

RowBatcher::~RowBatcher() {
    std::lock_guard<std::mutex> lock(futures_mutex_);
    for (auto& f : pending_flushes_)
        f.wait();
}

void RowBatcher::Append(const EncodedRow& buf) {
    std::shared_ptr<arrow::Table> table;
    {
        std::lock_guard<std::mutex> lock(append_mutex_);
        DoAppend(buf);
        if (++row_count_ >= batch_size_) {
            table = DoFlush();
            row_count_ = 0;
        }
    }
    if (table)
        DispatchFlush(std::move(table));
}

void RowBatcher::DispatchFlush(std::shared_ptr<arrow::Table> table) {
    auto future = std::async(std::launch::async,
        [this, t = std::move(table)]() mutable {
            if (on_flush_(std::move(t)))
                OnBatchFlushSucceeded();
            else
                OnBatchFlushFailed();
        });

    std::lock_guard<std::mutex> lock(futures_mutex_);
    // Prune futures that have already completed.
    pending_flushes_.erase(
        std::remove_if(pending_flushes_.begin(), pending_flushes_.end(),
            [](const std::future<void>& f) {
                return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
        pending_flushes_.end());
    pending_flushes_.push_back(std::move(future));
}

}  // namespace arrow_row
