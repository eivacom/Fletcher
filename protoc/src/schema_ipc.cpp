// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Must stay in sync with SerializeSchemaIpc in pubsub/src/schema_ipc.cpp:
// the .ipc files this plugin writes are documented as byte-identical to the
// schema bytes providers announce at runtime, and both sides vendor the
// same nanoarrow amalgamation to keep that guarantee.
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include "schema_builder.hpp"

namespace fletcher {

namespace {

void Check(ArrowErrorCode code, const ArrowError* err, const char* context) {
    if (code == NANOARROW_OK) return;
    std::string msg = context;
    if (err && err->message[0]) msg += std::string(": ") + err->message;
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
    Check(ArrowIpcOutputStreamInitBuffer(&stream, &buf), &err,
          "SerializeSchemaIpc: init output stream");

    // Writer.
    ArrowIpcWriter writer;
    std::memset(&writer, 0, sizeof(writer));
    Check(ArrowIpcWriterInit(&writer, &stream), &err, "SerializeSchemaIpc: init writer");

    // Write schema message.
    Check(ArrowIpcWriterWriteSchema(&writer, schema, &err), &err,
          "SerializeSchemaIpc: write schema");

    // Write EOS (null array view signals end of stream).
    Check(ArrowIpcWriterWriteArrayView(&writer, nullptr, &err), &err,
          "SerializeSchemaIpc: write EOS");

    ArrowIpcWriterReset(&writer);
    if (stream.release) stream.release(&stream);

    // Copy out.
    std::vector<uint8_t> result(buf.data, buf.data + buf.size_bytes);
    ArrowBufferReset(&buf);
    return result;
}

}  // namespace fletcher
