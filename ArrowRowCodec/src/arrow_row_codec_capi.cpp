#include "arrow_row_codec_capi.h"

#include "row_codec.hpp"

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

}  // namespace

// ---------------------------------------------------------------------------
// C API implementation
// ---------------------------------------------------------------------------

extern "C" {

void arrow_row_free_string(char* s)      { delete[] s; }
void arrow_row_free_bytes(uint8_t* data) { delete[] data; }

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

}  // extern "C"
