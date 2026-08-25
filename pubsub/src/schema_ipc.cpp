// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/schema_ipc.hpp"

#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace fletcher {

namespace {

void Check(ArrowErrorCode code, const ArrowError* err, const char* context) {
    if (code == NANOARROW_OK) return;
    std::string msg = context;
    if (err && err->message[0]) msg += std::string(": ") + err->message;
    throw std::runtime_error(msg);
}

// Runs a cleanup on scope exit, or earlier via Run(). Nanoarrow hands back C resources with no
// destructor, and every step here can throw.
class Guard {
   public:
    explicit Guard(std::function<void()> cleanup) : cleanup_(std::move(cleanup)) {}
    ~Guard() { Run(); }

    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    void Run() {
        if (!cleanup_) return;
        auto cleanup = std::move(cleanup_);
        cleanup_ = nullptr;
        cleanup();
    }

   private:
    std::function<void()> cleanup_;
};

}  // anonymous namespace

std::vector<uint8_t> SerializeSchemaIpc(const ArrowSchema* schema) {
    ArrowError err;
    std::memset(&err, 0, sizeof(err));

    // Every Check below can throw, so each resource is owned by a guard rather than released at the
    // end of the happy path. Ownership migrates as nanoarrow takes it over:
    // ArrowIpcOutputStreamInitBuffer makes the stream own the buffer, and ArrowIpcWriterInit moves
    // the stream into the writer (zeroing ours), so a released guard whose resource has moved on is
    // a no-op.
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    Guard buf_guard([&] { ArrowBufferReset(&buf); });

    ArrowIpcOutputStream stream;
    std::memset(&stream, 0, sizeof(stream));
    Guard stream_guard([&] {
        if (stream.release) stream.release(&stream);
    });
    Check(ArrowIpcOutputStreamInitBuffer(&stream, &buf), &err,
          "SerializeSchemaIpc: init output stream");

    ArrowIpcWriter writer;
    std::memset(&writer, 0, sizeof(writer));
    Guard writer_guard([&] { ArrowIpcWriterReset(&writer); });
    Check(ArrowIpcWriterInit(&writer, &stream), &err, "SerializeSchemaIpc: init writer");

    // Write schema message.
    Check(ArrowIpcWriterWriteSchema(&writer, schema, &err), &err,
          "SerializeSchemaIpc: write schema");

    // Write EOS (null array view signals end of stream).
    Check(ArrowIpcWriterWriteArrayView(&writer, nullptr, &err), &err,
          "SerializeSchemaIpc: write EOS");

    // Flush the writer before reading the buffer it wrote into.
    writer_guard.Run();
    stream_guard.Run();

    return std::vector<uint8_t>(buf.data, buf.data + buf.size_bytes);
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
    if (ec != NANOARROW_OK) throw std::runtime_error("DeserializeSchemaIpc: get_schema failed");

    return result;
}

}  // namespace fletcher
