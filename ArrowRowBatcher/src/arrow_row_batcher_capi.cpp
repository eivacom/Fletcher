#include "arrow_row_batcher_capi.h"

#include "generic_row_batcher.hpp"
#include "row_codec.hpp"
#include "sqlite_wal.hpp"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

template <typename T>
T Unwrap(arrow::Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.status().ToString());
    return std::move(*result);
}

void SetError(char** out_error, const std::string& msg) {
    if (!out_error) return;
    *out_error = new char[msg.size() + 1];
    std::memcpy(*out_error, msg.c_str(), msg.size() + 1);
}

std::shared_ptr<arrow::Schema> DeserializeSchema(const uint8_t* data, size_t len) {
    auto buf    = std::make_shared<arrow::Buffer>(data, static_cast<int64_t>(len));
    auto reader = Unwrap(arrow::ipc::RecordBatchStreamReader::Open(
        std::make_shared<arrow::io::BufferReader>(buf)));
    return reader->schema();
}

std::vector<uint8_t> SerializeTable(const arrow::Table& table) {
    auto sink   = Unwrap(arrow::io::BufferOutputStream::Create());
    auto writer = Unwrap(arrow::ipc::MakeStreamWriter(sink, table.schema()));
    auto reader = std::make_shared<arrow::TableBatchReader>(table);
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
        auto st = reader->ReadNext(&batch);
        if (!st.ok()) throw std::runtime_error(st.ToString());
        if (!batch) break;
        st = writer->WriteRecordBatch(*batch);
        if (!st.ok()) throw std::runtime_error(st.ToString());
    }
    auto st = writer->Close();
    if (!st.ok()) throw std::runtime_error(st.ToString());
    auto buf = Unwrap(sink->Finish());
    return std::vector<uint8_t>(buf->data(), buf->data() + buf->size());
}

}  // namespace

// ---------------------------------------------------------------------------
// C API implementation
// ---------------------------------------------------------------------------

extern "C" {

ArrowRowSQLiteWAL* arrow_row_sqlite_wal_new(const char* path, char** out_error) {
    try {
        return reinterpret_cast<ArrowRowSQLiteWAL*>(
            new fletcher::SQLiteWAL(path));
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return nullptr;
    }
}

void arrow_row_sqlite_wal_free(ArrowRowSQLiteWAL* wal) {
    delete reinterpret_cast<fletcher::SQLiteWAL*>(wal);
}

ArrowRowBatcher* arrow_row_batcher_new(ArrowRowSQLiteWAL* wal,
                                        const uint8_t*     schema_ipc,
                                        size_t             schema_ipc_len,
                                        int64_t            batch_size,
                                        ArrowRowFlushFn    on_flush,
                                        void*              userdata,
                                        char**             out_error) {
    try {
        auto schema   = DeserializeSchema(schema_ipc, schema_ipc_len);
        auto* wal_obj = reinterpret_cast<fletcher::SQLiteWAL*>(wal);

        fletcher::RowBatcher::FlushCallback cpp_callback =
            [on_flush, userdata](std::shared_ptr<arrow::Table> table) -> bool {
                auto ipc = SerializeTable(*table);
                return on_flush(ipc.data(), ipc.size(), userdata);
            };

        return reinterpret_cast<ArrowRowBatcher*>(
            new fletcher::GenericRowBatcher(
                std::move(schema), *wal_obj, batch_size, std::move(cpp_callback)));
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return nullptr;
    }
}

void arrow_row_batcher_free(ArrowRowBatcher* batcher) {
    delete reinterpret_cast<fletcher::GenericRowBatcher*>(batcher);
}

bool arrow_row_batcher_append(ArrowRowBatcher* batcher,
                               const uint8_t*   row_data,
                               size_t           row_len,
                               char**           out_error) {
    try {
        fletcher::EncodedRow row(row_data, row_data + row_len);
        reinterpret_cast<fletcher::GenericRowBatcher*>(batcher)->Append(row);
        return true;
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return false;
    }
}

int64_t arrow_row_batcher_row_count(const ArrowRowBatcher* batcher) {
    return reinterpret_cast<const fletcher::GenericRowBatcher*>(batcher)->row_count();
}

}  // extern "C"
