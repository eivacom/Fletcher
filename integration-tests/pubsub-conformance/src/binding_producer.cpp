// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-2d: the copy oracle's BINDING producer — the shipped codec, reached
// through the shipped C ABI, scored by the same ledger as every other leg.
//
// ── What this retires ───────────────────────────────────────────────────────
// The README's standing caveat on `encode_copies == 0` says the number "claims
// what the interface PERMITS, and no more … none exists, so the producer
// measured here is a stand-in written in this harness". That was true while no
// binding existed. One does now: `fletcher-c-abi` is the artifact
// `Eiva.Fletcher.Interop` ships per RID, and this file drives it — `fl_codec_open`,
// `fl_rows_bind`, `fl_encode_row` — into a span of the PROBE'S OWN window.
//
// ── What it does not claim, and why the difference is structural ────────────
// It does not run `fl_publisher_publish_row`. It cannot: the oracle samples
// `produced_at` from inside the `RowEncoder` frame, so it can only measure a
// producer it hands a `WriteBuffer` to, and `SeamProbeProvider` — the
// instrumented provider the whole ledger is built around — is a harness-owned
// `PubSubProvider` that D-BIND-24 deliberately forbids anything outside from
// registering in the shim's builtin registry. The fusion resolves its own
// provider and owns its own encoder frame. Ruled D-BIND-34.
//
// What the fusion adds over what is measured here is that `Publish` hands the
// PROVIDER's window to the same encoder. That step moves no bytes; it is
// structural in `c-abi/src/binding.cpp` and covered by BIND-2c's tests. A copy
// claim this instrument cannot score is not improved by routing it through code
// that does not touch the bytes.
//
// ── Two copies of Fletcher in this process, and why it is safe HERE ─────────
// The shim statically links its own `fletcher-pubsub`, and this harness links
// another. That is the very shape D-BIND-17's single-copy rule exists to
// prevent — every separately linked copy gets its own delivery-frame
// `thread_local`, so the re-entrancy doors stop seeing each other's frames.
// Nothing in this file crosses that line: `fl_encode_row` touches no provider,
// takes no seam lock and enters no delivery, and the probe's publish path runs
// entirely inside the harness's copy. The shim's marker check does not fire
// because this binary is not a second shim — it exports no marker.
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "fletcher/abi/binding.h"
#include "fletcher/conformance/copy_accounting.hpp"

namespace fletcher::conformance {
namespace {

/// The one-field row this leg publishes, and the reason it is a `binary` field.
///
/// The ledger checks `produced_len` and `delivered_len` against an exact byte
/// count, so the leg needs a row whose encoding is exactly `row_bytes`. The wire
/// format gives a single non-null binary field `[NULL_BITFIELD : 1][LEN : u32]
/// [bytes]`, so a payload of `row_bytes - 5` encodes to exactly `row_bytes` —
/// arithmetic rather than a magic constant, and it holds for both of the
/// oracle's row sizes.
constexpr size_t kRowOverheadBytes = 1 + 4;

[[noreturn]] void Fail(const std::string& what) {
    throw std::runtime_error("BindingProducer: " + what);
}

void Ok(int code, const char* what) {
    if (code != NANOARROW_OK) {
        Fail(std::string(what) + " failed with nanoarrow code " + std::to_string(code));
    }
}

/// Everything the ABI needs for one bound batch, released in the order the ABI
/// requires: unbind, then close, then the caller's own export.
class BoundRow {
   public:
    explicit BoundRow(const std::vector<uint8_t>& value) {
        ArrowError arrow_error;
        Ok(ArrowSchemaInitFromType(&schema_, NANOARROW_TYPE_STRUCT), "schema init");
        Ok(ArrowSchemaAllocateChildren(&schema_, 1), "schema children");
        Ok(ArrowSchemaInitFromType(schema_.children[0], NANOARROW_TYPE_BINARY), "child init");
        Ok(ArrowSchemaSetName(schema_.children[0], "payload"), "child name");
        // Non-nullable: a null field would encode as a bit and no payload, and
        // the arithmetic above assumes the value is present.
        schema_.children[0]->flags &= ~ARROW_FLAG_NULLABLE;

        Ok(ArrowArrayInitFromSchema(&array_, &schema_, &arrow_error), "array init");
        Ok(ArrowArrayStartAppending(&array_), "array start");
        ArrowBufferView view;
        view.data.as_uint8 = value.data();
        view.size_bytes = static_cast<int64_t>(value.size());
        Ok(ArrowArrayAppendBytes(array_.children[0], view), "append bytes");
        Ok(ArrowArrayFinishElement(&array_), "finish element");
        Ok(ArrowArrayFinishBuildingDefault(&array_, &arrow_error), "finish building");

        fl_error err = {};
        if (fl_codec_open(&schema_, &codec_, &err) != FL_OK) Fail(Message(err));
        if (fl_rows_bind(codec_, &array_, &rows_, &err) != FL_OK) Fail(Message(err));
    }

    ~BoundRow() {
        if (rows_ != nullptr) fl_rows_unbind(rows_);
        if (codec_ != nullptr) fl_codec_close(codec_);
        if (array_.release != nullptr) array_.release(&array_);
        if (schema_.release != nullptr) schema_.release(&schema_);
    }

    BoundRow(const BoundRow&) = delete;
    BoundRow& operator=(const BoundRow&) = delete;

    /// Encode row 0 into `[dst, dst + room)` and return the bytes written.
    ///
    /// A FIXED-capacity window (`grow == nullptr`) on purpose: `AppendInPlace`
    /// has already guaranteed `room >= min_bytes`, so a refill here would mean
    /// the row did not fit the span the oracle lent — which is a failure worth
    /// seeing as FL_PAYLOAD_TOO_LARGE rather than papering over by growing a
    /// buffer the probe does not own.
    size_t EncodeInto(uint8_t* dst, size_t room) const {
        fl_write_window window = {};
        window.data = dst;
        window.capacity = room;
        window.pos = 0;
        window.grow = nullptr;

        fl_error err = {};
        if (fl_encode_row(rows_, 0, &window, &err) != FL_OK) Fail(Message(err));
        return window.pos;
    }

   private:
    /// The ABI's message, and then disposed — this side is the caller, so the
    /// message is the shim's to allocate and ours to release (D-BIND-32 governs
    /// the other direction, where a callback fills the error).
    static std::string Message(fl_error& err) {
        std::string text(reinterpret_cast<const char*>(err.message), err.message_len);
        fl_error_dispose(&err);
        return text.empty() ? "the ABI refused without a message" : text;
    }

    ArrowSchema schema_ = {};
    ArrowArray array_ = {};
    fl_codec* codec_ = nullptr;
    fl_rows* rows_ = nullptr;
};

}  // namespace

std::vector<uint8_t> BindingRowValue(size_t row_bytes) {
    if (row_bytes <= kRowOverheadBytes) {
        Fail("row_bytes " + std::to_string(row_bytes) + " is too small to hold a binary field");
    }
    return CopyPayload(row_bytes - kRowOverheadBytes);
}

RoundTrip RunBindingProducerRoundTrip(CopyRunner& runner, const Topic& topic, size_t row_bytes) {
    const std::vector<uint8_t> value = BindingRowValue(row_bytes);
    const BoundRow row(value);

    // The expected bytes are the ABI's OWN encoding, produced once into scratch
    // storage the probe never sees. That is what keeps the delivery's
    // row-content check real: the leg is not asserting against a pattern it
    // chose, it is asserting that what arrived is what this codec encodes. The
    // encode is deterministic, so the in-place run below must reproduce it byte
    // for byte or the content check fails — which is a second, free guard on the
    // in-place path.
    std::vector<uint8_t> expected(row_bytes);
    const size_t written = row.EncodeInto(expected.data(), expected.size());
    if (written != row_bytes) {
        Fail("the encoded row is " + std::to_string(written) + " bytes, not the " +
             std::to_string(row_bytes) + " this leg's arithmetic predicts");
    }

    return RunCustomProducerRoundTrip(runner, topic, expected,
                                      [&row](uint8_t* dst, size_t room) -> ProducedRow {
                                          // Reported as `dst` because that is where
                                          // `fl_encode_row` wrote: its window IS the
                                          // lent span, fixed capacity, with no
                                          // intermediate buffer anywhere.
                                          return {dst, row.EncodeInto(dst, room)};
                                      });
}

RoundTrip RunBindingStagingProducerRoundTrip(CopyRunner& runner, const Topic& topic,
                                             size_t row_bytes) {
    const std::vector<uint8_t> value = BindingRowValue(row_bytes);
    const BoundRow row(value);

    std::vector<uint8_t> expected(row_bytes);
    if (row.EncodeInto(expected.data(), expected.size()) != row_bytes) {
        Fail("the staged control encoded a row of the wrong size");
    }

    // The control, and why it has to exist: the leg above SELF-REPORTS where it
    // wrote, because this harness cannot see inside a producer it does not own.
    // A leg that reported the lent span no matter what the producer did would
    // score zero by construction — a tautology wearing a measurement's clothes.
    // Here the same ABI composes the row in storage of its own and copies it in,
    // which is the whole-row copy a binding pays for ignoring the lent span, and
    // reports where it actually composed it.
    return RunCustomProducerRoundTrip(
        runner, topic, expected, [&row, row_bytes](uint8_t* dst, size_t room) -> ProducedRow {
            // Static so the address is still live when the ledger is read after
            // the lend returns; a stack buffer's address would dangle.
            static std::vector<uint8_t> staged;
            staged.assign(row_bytes, 0);
            const size_t written = row.EncodeInto(staged.data(), staged.size());
            if (written > room) Fail("the staged row does not fit the lent span");
            std::memcpy(dst, staged.data(), written);
            return {staged.data(), written};
        });
}

}  // namespace fletcher::conformance
