// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The C++ half of the binding ABI conformance suite: emit the corpus.
//
// ── What this proves, once C# consumes what it writes ───────────────────────
// Every Fletcher binding calls ONE codec, so "do C# and C++ encode the same
// bytes?" cannot be answered by comparing two encoders — there is only one. The
// question that IS open, and that this suite answers, is one layer up:
//
//   Does `Apache.Arrow`'s C# export of a batch produce, through that one codec,
//   the same wire bytes that Arrow C++'s export of the same batch produces?
//
// Two independent Arrow implementations build the array, lay out its buffers,
// and hand it across the C Data Interface. They agree about offsets, validity
// bitmaps, child ordering and buffer padding — or they do not, and the codec
// reads the difference straight onto the wire, silently, as a subscriber
// decoding a publisher's row into the wrong values.
//
// So this writes, per fixture: the batch as an Arrow IPC stream (what C# rebuilds
// its own arrays from) and the rows as this side encoded them (what C# must
// reproduce byte for byte).
//
// ── Why the corpus is included rather than rebuilt ──────────────────────────
// `codec_corpus.hpp` is the same corpus `NanoarrowCodec.ByteIdenticalToArrowBridge`
// compares the two C++ encoders over, which D-BIND-35 ruled the right shape for
// this job. A corpus written again here would drift from that one, and both would
// go on passing while covering different things.
//
// ── Why the bytes come through the C ABI, not through NanoarrowCodec ────────
// This is the BINDING ABI's conformance suite. A binding reaches the codec
// through `fl_codec_open` / `fl_rows_bind` / `fl_encode_row` and nothing else, so
// the reference bytes are produced the same way — otherwise the suite would
// certify a path no binding takes.
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "codec_corpus.hpp"
#include "fletcher/abi/binding.h"

namespace {

using fletcher::abi::corpus::Corpus;
using fletcher::abi::corpus::Fixture;

/// A failure here is a broken emitter, not a failed comparison. Say which.
[[noreturn]] void Die(const std::string& what) {
    std::cerr << "emit_corpus: " << what << "\n";
    std::exit(1);
}

std::string MessageOf(const fl_error& err) {
    if (err.message == nullptr) return "(no message)";
    return std::string(reinterpret_cast<const char*>(err.message), err.message_len);
}

/// A growable window over a `std::vector`, the shape `fl_encode_row` refills.
struct VectorWindow {
    std::vector<uint8_t> storage;
    fl_write_window window = {};

    VectorWindow() {
        storage.resize(256);
        window.ctx = this;
        window.data = storage.data();
        window.capacity = storage.size();
        window.pos = 0;
        window.grow = &VectorWindow::Grow;
    }

    static fl_status Grow(fl_write_window* w, size_t min_bytes, fl_error* /*err*/) {
        auto* self = static_cast<VectorWindow*>(w->ctx);
        self->storage.resize(std::max(self->storage.size() * 2, w->pos + min_bytes));
        w->data = self->storage.data();
        w->capacity = self->storage.size();
        return FL_OK;
    }
};

/// Every row of `batch`, encoded back to back THROUGH THE ABI.
std::vector<uint8_t> EncodeAllRowsThroughTheAbi(const arrow::RecordBatch& batch) {
    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    if (!arrow::ExportRecordBatch(batch, &c_array, &c_schema).ok()) {
        Die("the fixture could not be exported across the C Data Interface");
    }

    fl_error err = {};
    fl_codec* codec = nullptr;
    if (fl_codec_open(&c_schema, &codec, &err) != FL_OK) {
        Die("fl_codec_open refused the fixture's schema: " + MessageOf(err));
    }

    fl_rows* rows = nullptr;
    if (fl_rows_bind(codec, &c_array, &rows, &err) != FL_OK) {
        Die("fl_rows_bind refused the fixture's batch: " + MessageOf(err));
    }

    VectorWindow sink;
    for (int64_t row = 0; row < batch.num_rows(); ++row) {
        if (fl_encode_row(rows, row, &sink.window, &err) != FL_OK) {
            Die("fl_encode_row failed on row " + std::to_string(row) + ": " + MessageOf(err));
        }
    }

    fl_rows_unbind(rows);
    fl_codec_close(codec);

    // The borrow rule, one level up: the ABI consumed neither export.
    if (c_array.release != nullptr) c_array.release(&c_array);
    if (c_schema.release != nullptr) c_schema.release(&c_schema);

    return {sink.storage.begin(), sink.storage.begin() + static_cast<ptrdiff_t>(sink.window.pos)};
}

void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) Die("could not open " + path.string() + " for writing");
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) Die("could not write " + path.string());
}

/// The batch as an Arrow IPC stream: schema and values in one file.
///
/// One file rather than two because the stream format already carries the schema,
/// and a separate schema file is one more thing that can disagree with the batch
/// beside it.
void WriteIpc(const std::filesystem::path& path, const arrow::RecordBatch& batch) {
    auto file = arrow::io::FileOutputStream::Open(path.string());
    if (!file.ok()) Die("could not open " + path.string() + ": " + file.status().ToString());

    auto writer = arrow::ipc::MakeStreamWriter(*file, batch.schema());
    if (!writer.ok()) Die("could not start an IPC stream: " + writer.status().ToString());
    if (!(*writer)->WriteRecordBatch(batch).ok())
        Die("could not write the batch to " + path.string());
    if (!(*writer)->Close().ok()) Die("could not close the IPC stream for " + path.string());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_corpus <output-directory>\n";
        return 2;
    }

    const std::filesystem::path out_dir(argv[1]);
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) Die("could not create " + out_dir.string() + ": " + ec.message());

    // The manifest is what makes the C# side's coverage DISCOVERED rather than
    // listed: a fixture added to the corpus appears here and is consumed, instead
    // of being silently skipped by a hand-written list on the other side.
    std::ofstream manifest(out_dir / "manifest.txt", std::ios::trunc);
    if (!manifest) Die("could not write the manifest");

    const std::vector<Fixture> corpus = Corpus();
    if (corpus.empty()) Die("the corpus is empty, so this suite would certify nothing");

    for (const Fixture& fixture : corpus) {
        if (fixture.batch->num_rows() == 0) {
            Die("fixture '" + fixture.name + "' has no rows, so round-tripping it proves nothing");
        }

        const std::vector<uint8_t> encoded = EncodeAllRowsThroughTheAbi(*fixture.batch);
        if (encoded.empty()) Die("fixture '" + fixture.name + "' encoded to nothing");

        WriteIpc(out_dir / (fixture.name + ".arrows"), *fixture.batch);
        WriteBytes(out_dir / (fixture.name + ".rows.bin"), encoded);

        manifest << fixture.name << " " << fixture.batch->num_rows() << " " << encoded.size()
                 << "\n";
        std::cout << "emitted " << fixture.name << ": " << fixture.batch->num_rows() << " rows, "
                  << encoded.size() << " bytes\n";
    }

    manifest.close();
    std::cout << "emitted " << corpus.size() << " fixtures to " << out_dir.string() << "\n";
    return 0;
}
