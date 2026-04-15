#include "pubsub/schema_ipc.hpp"

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace fletcher {

namespace {

void Check(ArrowErrorCode code, const ArrowError* err, const char* context) {
    if (code == NANOARROW_OK) return;
    std::string msg = context;
    if (err && err->message[0])
        msg += std::string(": ") + err->message;
    throw std::runtime_error(msg);
}

}  // anonymous namespace

std::vector<uint8_t> SerializeSchemaIpc(const ArrowSchema* schema) {
    ArrowError err;
    std::memset(&err, 0, sizeof(err));

    // Output buffer.
    ArrowBuffer buf;
    ArrowBufferInit(&buf);

    // Output stream backed by the buffer.
    ArrowIpcOutputStream stream;
    std::memset(&stream, 0, sizeof(stream));
    Check(ArrowIpcOutputStreamInitBuffer(&stream, &buf),
          &err, "SerializeSchemaIpc: init output stream");

    // Writer.
    ArrowIpcWriter writer;
    std::memset(&writer, 0, sizeof(writer));
    Check(ArrowIpcWriterInit(&writer, &stream),
          &err, "SerializeSchemaIpc: init writer");

    // Write schema message.
    Check(ArrowIpcWriterWriteSchema(&writer, schema, &err),
          &err, "SerializeSchemaIpc: write schema");

    // Write EOS (null array view signals end of stream).
    Check(ArrowIpcWriterWriteArrayView(&writer, nullptr, &err),
          &err, "SerializeSchemaIpc: write EOS");

    ArrowIpcWriterReset(&writer);
    if (stream.release) stream.release(&stream);

    // Copy out.
    std::vector<uint8_t> result(buf.data, buf.data + buf.size_bytes);
    ArrowBufferReset(&buf);
    return result;
}

OwnedSchema DeserializeSchemaIpc(const uint8_t* data, size_t len) {
    // Wrap data into an ArrowBuffer (copy, since ArrowIpcInputStreamInitBuffer
    // takes ownership).
    ArrowBuffer input_buf;
    ArrowBufferInit(&input_buf);
    ArrowErrorCode ec = ArrowBufferAppend(&input_buf, data, static_cast<int64_t>(len));
    if (ec != NANOARROW_OK) {
        ArrowBufferReset(&input_buf);
        throw std::runtime_error("DeserializeSchemaIpc: buffer alloc failed");
    }

    // Input stream.
    ArrowIpcInputStream stream;
    std::memset(&stream, 0, sizeof(stream));
    ec = ArrowIpcInputStreamInitBuffer(&stream, &input_buf);
    if (ec != NANOARROW_OK) {
        ArrowBufferReset(&input_buf);
        throw std::runtime_error("DeserializeSchemaIpc: init input stream");
    }
    // stream now owns input_buf.

    // ArrayStream reader — reads schema + batches.
    ArrowArrayStream array_stream;
    std::memset(&array_stream, 0, sizeof(array_stream));
    ec = ArrowIpcArrayStreamReaderInit(&array_stream, &stream, nullptr);
    if (ec != NANOARROW_OK) {
        if (stream.release) stream.release(&stream);
        throw std::runtime_error("DeserializeSchemaIpc: init reader");
    }
    // array_stream now owns stream.

    // Extract schema.
    OwnedSchema result;
    ec = array_stream.get_schema(&array_stream, result.get());
    array_stream.release(&array_stream);
    if (ec != NANOARROW_OK)
        throw std::runtime_error("DeserializeSchemaIpc: get_schema failed");

    return result;
}

}  // namespace fletcher
