#include "pubsub/schema_ipc.hpp"

#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>

#include <stdexcept>

namespace fletcher {

namespace {

template <typename T>
T Unwrap(arrow::Result<T> result) {
    if (!result.ok())
        throw std::runtime_error(result.status().ToString());
    return std::move(*result);
}

}  // namespace

std::vector<uint8_t> SerializeSchemaIpc(const arrow::Schema& schema) {
    auto sink   = Unwrap(arrow::io::BufferOutputStream::Create());
    auto writer = Unwrap(arrow::ipc::MakeStreamWriter(
        sink, std::make_shared<arrow::Schema>(schema)));
    auto st = writer->Close();
    if (!st.ok()) throw std::runtime_error(st.ToString());

    auto buf = Unwrap(sink->Finish());
    return {buf->data(), buf->data() + buf->size()};
}

std::shared_ptr<arrow::Schema> DeserializeSchemaIpc(const uint8_t* data,
                                                     size_t len) {
    auto buf    = std::make_shared<arrow::Buffer>(data, static_cast<int64_t>(len));
    auto reader = Unwrap(arrow::ipc::RecordBatchStreamReader::Open(
        std::make_shared<arrow::io::BufferReader>(buf)));
    return reader->schema();
}

}  // namespace fletcher
