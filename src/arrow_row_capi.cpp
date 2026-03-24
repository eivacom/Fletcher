#include "arrow_row_capi.h"

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

// Unwrap an arrow::Result, throwing std::runtime_error on failure.
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

// Read the schema from a schema-only Arrow IPC stream.
std::shared_ptr<arrow::Schema> DeserializeSchema(const uint8_t* data, size_t len) {
    auto buf    = std::make_shared<arrow::Buffer>(data, static_cast<int64_t>(len));
    auto reader = Unwrap(arrow::ipc::RecordBatchStreamReader::Open(
        std::make_shared<arrow::io::BufferReader>(buf)));
    return reader->schema();
}

// Read the first record batch from an Arrow IPC stream.
std::shared_ptr<arrow::RecordBatch> DeserializeBatch(const uint8_t* data, size_t len) {
    auto buf    = std::make_shared<arrow::Buffer>(data, static_cast<int64_t>(len));
    auto reader = Unwrap(arrow::ipc::RecordBatchStreamReader::Open(
        std::make_shared<arrow::io::BufferReader>(buf)));
    std::shared_ptr<arrow::RecordBatch> batch;
    auto st = reader->ReadNext(&batch);
    if (!st.ok()) throw std::runtime_error(st.ToString());
    if (!batch)   throw std::runtime_error("IPC stream contained no record batches");
    return batch;
}

// Serialize a record batch to an Arrow IPC stream.
std::vector<uint8_t> SerializeBatch(const arrow::RecordBatch& batch) {
    auto sink   = Unwrap(arrow::io::BufferOutputStream::Create());
    auto writer = Unwrap(arrow::ipc::MakeStreamWriter(sink, batch.schema()));
    auto st = writer->WriteRecordBatch(batch);
    if (!st.ok()) throw std::runtime_error(st.ToString());
    st = writer->Close();
    if (!st.ok()) throw std::runtime_error(st.ToString());
    auto buf = Unwrap(sink->Finish());
    return std::vector<uint8_t>(buf->data(), buf->data() + buf->size());
}

// Serialize an Arrow Table to an Arrow IPC stream (all chunks combined).
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

void arrow_row_free_string(char* s) { delete[] s; }
void arrow_row_free_bytes(uint8_t* data) { delete[] data; }

// --- RowCodec ----------------------------------------------------------------

ArrowRowCodec* arrow_row_codec_new(const uint8_t* schema_ipc,
                                    size_t         schema_ipc_len,
                                    char**         out_error) {
    try {
        auto schema = DeserializeSchema(schema_ipc, schema_ipc_len);
        return reinterpret_cast<ArrowRowCodec*>(
            new arrow_row::RowCodec(std::move(schema)));
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return nullptr;
    }
}

void arrow_row_codec_free(ArrowRowCodec* codec) {
    delete reinterpret_cast<arrow_row::RowCodec*>(codec);
}

bool arrow_row_codec_encode_row(const ArrowRowCodec* codec,
                                 const uint8_t*       ipc_data,
                                 size_t               ipc_len,
                                 uint8_t**            out_data,
                                 size_t*              out_len,
                                 char**               out_error) {
    try {
        const auto* c = reinterpret_cast<const arrow_row::RowCodec*>(codec);
        auto batch = DeserializeBatch(ipc_data, ipc_len);

        // Extract one scalar per column (row 0).
        std::vector<std::shared_ptr<arrow::Scalar>> scalars;
        scalars.reserve(batch->num_columns());
        for (int i = 0; i < batch->num_columns(); ++i)
            scalars.push_back(Unwrap(batch->column(i)->GetScalar(0)));

        auto row = c->EncodeRow(scalars);
        auto* buf = new uint8_t[row.size()];
        std::memcpy(buf, row.data(), row.size());
        *out_data = buf;
        *out_len  = row.size();
        return true;
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return false;
    }
}

bool arrow_row_codec_decode_row(const ArrowRowCodec* codec,
                                 const uint8_t*       row_data,
                                 size_t               row_len,
                                 uint8_t**            out_ipc,
                                 size_t*              out_ipc_len,
                                 char**               out_error) {
    try {
        const auto* c = reinterpret_cast<const arrow_row::RowCodec*>(codec);
        arrow_row::ArrowRow row(row_data, row_data + row_len);
        auto scalars = c->DecodeRow(row);

        const arrow::Schema& schema = c->schema();
        auto schema_ptr = arrow::schema(schema.fields(), schema.metadata());

        arrow::ArrayVector arrays;
        arrays.reserve(schema.num_fields());
        for (int i = 0; i < schema.num_fields(); ++i)
            arrays.push_back(Unwrap(arrow::MakeArrayFromScalar(*scalars[i], 1)));

        auto batch = arrow::RecordBatch::Make(schema_ptr, 1, std::move(arrays));
        auto ipc   = SerializeBatch(*batch);
        auto* buf  = new uint8_t[ipc.size()];
        std::memcpy(buf, ipc.data(), ipc.size());
        *out_ipc     = buf;
        *out_ipc_len = ipc.size();
        return true;
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return false;
    }
}

// --- SQLiteWAL ---------------------------------------------------------------

ArrowRowSQLiteWAL* arrow_row_sqlite_wal_new(const char* path, char** out_error) {
    try {
        return reinterpret_cast<ArrowRowSQLiteWAL*>(
            new arrow_row::SQLiteWAL(path));
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return nullptr;
    }
}

void arrow_row_sqlite_wal_free(ArrowRowSQLiteWAL* wal) {
    delete reinterpret_cast<arrow_row::SQLiteWAL*>(wal);
}

// --- GenericRowBatcher -------------------------------------------------------

ArrowRowBatcher* arrow_row_batcher_new(ArrowRowSQLiteWAL* wal,
                                        const uint8_t*     schema_ipc,
                                        size_t             schema_ipc_len,
                                        int64_t            batch_size,
                                        ArrowRowFlushFn    on_flush,
                                        void*              userdata,
                                        char**             out_error) {
    try {
        auto schema = DeserializeSchema(schema_ipc, schema_ipc_len);
        auto* wal_obj = reinterpret_cast<arrow_row::SQLiteWAL*>(wal);

        arrow_row::RowBatcher::FlushCallback cpp_callback =
            [on_flush, userdata](std::shared_ptr<arrow::Table> table) -> bool {
                auto ipc = SerializeTable(*table);
                return on_flush(ipc.data(), ipc.size(), userdata);
            };

        return reinterpret_cast<ArrowRowBatcher*>(
            new arrow_row::GenericRowBatcher(
                std::move(schema), *wal_obj, batch_size, std::move(cpp_callback)));
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return nullptr;
    }
}

void arrow_row_batcher_free(ArrowRowBatcher* batcher) {
    delete reinterpret_cast<arrow_row::GenericRowBatcher*>(batcher);
}

bool arrow_row_batcher_append(ArrowRowBatcher* batcher,
                               const uint8_t*   row_data,
                               size_t           row_len,
                               char**           out_error) {
    try {
        arrow_row::ArrowRow row(row_data, row_data + row_len);
        reinterpret_cast<arrow_row::GenericRowBatcher*>(batcher)->Append(row);
        return true;
    } catch (const std::exception& e) {
        SetError(out_error, e.what());
        return false;
    }
}

int64_t arrow_row_batcher_row_count(const ArrowRowBatcher* batcher) {
    return reinterpret_cast<const arrow_row::GenericRowBatcher*>(batcher)->row_count();
}

}  // extern "C"
