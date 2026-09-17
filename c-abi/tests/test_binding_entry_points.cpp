// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-2c's forcing tests: the ABI, exercised THROUGH THE SHIM'S EXPORT TABLE.
//
// ── Why that distinction is the whole design of this file ───────────────────
// `test_nanoarrow_codec.cpp` links the codec's objects directly and tests the
// C++ class. This file does not: every call below goes out through
// `fletcher-c-abi`'s import library and comes back, so what is under test is the
// surface that SHIPS — the containment site, the handle lifetimes, the status
// numbers and the message bytes a binding will actually see. A defect that
// exists only at the boundary (an exception escaping a C function, a status
// invented instead of forwarded, a message lost on the way out) is invisible to
// the codec's own suite and lands here.
//
// ── The two properties worth stating ────────────────────────────────────────
//   1. NOTHING IS RE-INVENTED AT THE BOUNDARY. A malformed row refused by the
//      positional reader arrives with the reader's own message; a malformed
//      topic refused by the seam arrives with the seam's. The ABI adds a number
//      and an origin and subtracts nothing.
//   2. THE ENCODER WRITES THROUGH. `fl_encode_row` into a caller's window
//      produces the bytes the codec produces, refills and all.
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <string>
#include <vector>

#include "../src/nanoarrow_codec.hpp"
#include "fletcher/abi/binding.h"

namespace {

using fletcher::abi::BoundRows;
using fletcher::abi::NanoarrowCodec;

/// A two-column batch: enough shape to exercise a length prefix and a null, few
/// enough bytes that a deliberately tiny window refills several times.
std::shared_ptr<arrow::RecordBatch> Fixture() {
    arrow::Int32Builder id;
    EXPECT_TRUE(id.AppendValues({1, 2, 3}).ok());

    arrow::StringBuilder label;
    EXPECT_TRUE(label.Append("alpha").ok());
    EXPECT_TRUE(label.AppendNull().ok());
    EXPECT_TRUE(label.Append("gamma").ok());

    std::shared_ptr<arrow::Array> id_array;
    std::shared_ptr<arrow::Array> label_array;
    EXPECT_TRUE(id.Finish(&id_array).ok());
    EXPECT_TRUE(label.Finish(&label_array).ok());

    auto schema =
        arrow::schema({arrow::field("id", arrow::int32()), arrow::field("label", arrow::utf8())});
    return arrow::RecordBatch::Make(schema, 3, {id_array, label_array});
}

/// The message an `fl_error` carries, as a string. Not NUL-terminated on the
/// wire, so the length is the only way to read it — which is also the rule the
/// ABI states for every string it hands out.
std::string MessageOf(const fl_error& err) {
    if (err.message == nullptr) return {};
    return {reinterpret_cast<const char*>(err.message), err.message_len};
}

/// A growable `fl_write_window` over a `std::vector`, and the smallest honest
/// implementation of the contract: `grow` preserves `pos` and the bytes below
/// it, and reports the new base and capacity.
struct VectorWindow {
    std::vector<uint8_t> storage;
    fl_write_window window = {};
    int grow_calls = 0;

    explicit VectorWindow(size_t initial) : storage(initial) {
        window.ctx = this;
        window.data = storage.data();
        window.capacity = storage.size();
        window.pos = 0;
        window.grow = &VectorWindow::Grow;
    }

    static fl_status Grow(fl_write_window* w, size_t min_bytes, fl_error*) {
        auto* self = static_cast<VectorWindow*>(w->ctx);
        ++self->grow_calls;
        self->storage.resize(w->pos + min_bytes + 8);  // +8 so refills are plural
        w->data = self->storage.data();
        w->capacity = self->storage.size();
        return FL_OK;
    }

    std::vector<uint8_t> Written() const {
        return {storage.begin(), storage.begin() + static_cast<ptrdiff_t>(window.pos)};
    }
};

/// The codec's own answer, from the copy of the codec linked into this test —
/// the oracle `fl_encode_row` is checked against.
std::vector<uint8_t> EncodeLocally(const arrow::RecordBatch& batch, int64_t row) {
    ArrowSchema c_schema = {};
    ArrowArray c_array = {};
    EXPECT_TRUE(arrow::ExportRecordBatch(batch, &c_array, &c_schema).ok());

    std::vector<uint8_t> bytes;
    {
        NanoarrowCodec codec(c_schema);
        BoundRows rows(codec, c_array);
        fletcher::VectorWriteBuffer buffer;
        codec.EncodeRow(rows, row, buffer);
        bytes = buffer.Finish();
    }
    c_array.release(&c_array);
    c_schema.release(&c_schema);
    return bytes;
}

/// Everything a test needs on the far side of the ABI: an open codec and a bound
/// batch, released in the right order (unbind, then the caller's own export).
class AbiFixture {
   public:
    AbiFixture() : batch_(Fixture()) {
        EXPECT_TRUE(arrow::ExportRecordBatch(*batch_, &array_, &schema_).ok());
        fl_error err = {};
        EXPECT_EQ(fl_codec_open(&schema_, &codec_, &err), FL_OK) << MessageOf(err);
        EXPECT_EQ(fl_rows_bind(codec_, &array_, &rows_, &err), FL_OK) << MessageOf(err);
    }

    ~AbiFixture() {
        fl_rows_unbind(rows_);
        fl_codec_close(codec_);
        // The borrow rule, one level up: the ABI never consumed the export, so
        // both structures are still ours to release.
        EXPECT_NE(array_.release, nullptr) << "fl_rows_bind consumed the caller's array";
        if (array_.release != nullptr) array_.release(&array_);
        if (schema_.release != nullptr) schema_.release(&schema_);
    }

    AbiFixture(const AbiFixture&) = delete;
    AbiFixture& operator=(const AbiFixture&) = delete;

    const arrow::RecordBatch& batch() const { return *batch_; }
    fl_codec* codec() const { return codec_; }
    fl_rows* rows() const { return rows_; }

   private:
    std::shared_ptr<arrow::RecordBatch> batch_;
    ArrowSchema schema_ = {};
    ArrowArray array_ = {};
    fl_codec* codec_ = nullptr;
    fl_rows* rows_ = nullptr;
};

fl_str Str(const char* s) { return fl_str{reinterpret_cast<const uint8_t*>(s), std::strlen(s)}; }

// ---------------------------------------------------------------------------
// The encoder writes through the caller's window
// ---------------------------------------------------------------------------

/// `fl_encode_row` produces the codec's bytes, and refills to get there.
///
/// The window starts at one byte on purpose. A row of this fixture does not fit,
/// so the adapter must go out through `grow` and come back — which is the path
/// that a naive adapter (one that reads `data`/`capacity` once and caches them)
/// gets wrong by writing through a stale pointer. `grow_calls` is asserted
/// non-zero so a future fixture that happens to fit cannot quietly retire the
/// check.
TEST(BindingEntryPoints, EncodeRowWritesTheCodecsBytesThroughAGrowingWindow) {
    AbiFixture abi;

    for (int64_t row = 0; row < abi.batch().num_rows(); ++row) {
        VectorWindow sink(1);
        fl_error err = {};
        ASSERT_EQ(fl_encode_row(abi.rows(), row, &sink.window, &err), FL_OK) << MessageOf(err);
        EXPECT_GT(sink.grow_calls, 0) << "row " << row
                                      << " fit a one-byte window, so this row "
                                         "never exercised a refill";
        EXPECT_EQ(sink.Written(), EncodeLocally(abi.batch(), row))
            << "row " << row << ": the ABI's bytes are not the codec's bytes";
    }
}

/// A fixed-capacity window that cannot hold the row is FL_PAYLOAD_TOO_LARGE.
///
/// `binding.h` promises exactly that number for this case, and it is reachable
/// only through the seam's normative `std::overflow_error` rule — so this row
/// also stands guard over that mapping surviving in the containment site.
TEST(BindingEntryPoints, AFixedWindowTooSmallForTheRowIsPayloadTooLarge) {
    AbiFixture abi;

    std::vector<uint8_t> storage(2);
    fl_write_window fixed = {};
    fixed.data = storage.data();
    fixed.capacity = storage.size();
    fixed.pos = 0;
    fixed.grow = nullptr;  // fixed-capacity

    fl_error err = {};
    EXPECT_EQ(fl_encode_row(abi.rows(), 0, &fixed, &err), FL_PAYLOAD_TOO_LARGE) << MessageOf(err);
    EXPECT_EQ(err.status, FL_PAYLOAD_TOO_LARGE);
    EXPECT_NE(MessageOf(err).find("fixed-capacity"), std::string::npos) << MessageOf(err);
    EXPECT_EQ(fixed.pos, 0U) << "a failed encode advanced the caller's cursor";
    fl_error_dispose(&err);
}

/// A `grow` that refuses keeps its own status and its own message, and is
/// attributed to the CALLER rather than to the seam.
TEST(BindingEntryPoints, AGrowThatRefusesKeepsItsStatusMessageAndOrigin) {
    AbiFixture abi;

    struct Refusing {
        static fl_status Grow(fl_write_window*, size_t, fl_error* err) {
            // What a binding's own grow thunk does: fill the error it was given.
            //
            // The bytes are STATIC on purpose. Who frees a message a CALLBACK put
            // in an `fl_error` is an open question against binding.h (see
            // write_window.hpp), so this test does not take a position on it by
            // allocating: storage that outlives the call is correct under every
            // reading of the rule.
            static const char kWhy[] = "the managed buffer would not grow";
            err->status = FL_TRANSPORT_FAILURE;
            err->origin = FL_ORIGIN_CALLBACK;
            err->message = reinterpret_cast<uint8_t*>(const_cast<char*>(kWhy));
            err->message_len = sizeof(kWhy) - 1;
            return FL_TRANSPORT_FAILURE;
        }
    };

    std::vector<uint8_t> storage(1);
    fl_write_window window = {};
    window.data = storage.data();
    window.capacity = storage.size();
    window.pos = 0;
    window.grow = &Refusing::Grow;

    fl_error err = {};
    EXPECT_EQ(fl_encode_row(abi.rows(), 0, &window, &err), FL_TRANSPORT_FAILURE);
    EXPECT_EQ(err.origin, FL_ORIGIN_CALLBACK)
        << "a refusal produced by the caller's own grow was attributed to the seam";
    EXPECT_NE(MessageOf(err).find("would not grow"), std::string::npos)
        << "the caller's own message was replaced: " << MessageOf(err);
    fl_error_dispose(&err);
}

// ---------------------------------------------------------------------------
// Nothing is re-invented at the boundary
// ---------------------------------------------------------------------------

/// A malformed buffer through `fl_decode_rows` carries the READER's message.
///
/// This is BIND-2b's parity property, raised to the ABI: the entry point is a
/// pass-through, so the binding inherits every bounds check the HARD rounds put
/// into `PositionalReader` rather than meeting a second taxonomy at the
/// boundary. The origin is what lets a binding raise its own typed format
/// exception for exactly this case (D-BIND-15).
TEST(BindingEntryPoints, MalformedBytesCarryTheReadersMessageAndTheCodecOrigin) {
    AbiFixture abi;

    VectorWindow sink(64);
    fl_error err = {};
    ASSERT_EQ(fl_encode_row(abi.rows(), 0, &sink.window, &err), FL_OK) << MessageOf(err);
    const std::vector<uint8_t> valid = sink.Written();
    ASSERT_FALSE(valid.empty());

    ArrowArray decoded = {};
    EXPECT_EQ(fl_decode_rows(abi.codec(), valid.data(), valid.size() - 1, 1, &decoded, &err),
              FL_INVALID_ARGUMENT);
    EXPECT_EQ(err.origin, FL_ORIGIN_CODEC);
    EXPECT_EQ(MessageOf(err).rfind("PositionalReader:", 0), 0U)
        << "the ABI replaced the reader's message instead of forwarding it: " << MessageOf(err);
    EXPECT_EQ(decoded.release, nullptr) << "a failed decode handed back a partial array";
    fl_error_dispose(&err);

    // The control: the untruncated buffer decodes through the same entry point.
    ASSERT_EQ(fl_decode_rows(abi.codec(), valid.data(), valid.size(), 1, &decoded, &err), FL_OK)
        << MessageOf(err);
    ASSERT_NE(decoded.release, nullptr);
    EXPECT_EQ(decoded.length, 1);
    decoded.release(&decoded);
}

/// A malformed topic carries the SEAM's message, for the same reason.
///
/// The shim keeps no second copy of the six topic rules: every publisher path
/// goes through `RequireSegments`, so the refusal that reaches a binding is the
/// one the seam wrote. A shim that restated the rules would pass this row too —
/// right up until the day the two copies disagreed.
TEST(BindingEntryPoints, AMalformedTopicCarriesTheSeamsRefusal) {
    fl_error err = {};
    fl_provider* provider = nullptr;
    const fl_provider_config config = {};
    ASSERT_EQ(fl_provider_create(Str("inprocess"), &config, &provider, &err), FL_OK)
        << MessageOf(err);

    fl_publisher* publisher = nullptr;
    ASSERT_EQ(fl_publisher_create(provider, &publisher, &err), FL_OK) << MessageOf(err);

    AbiFixture abi;
    ArrowSchema schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*abi.batch().schema(), &schema).ok());

    const fl_topic empty = {nullptr, 0};
    EXPECT_EQ(fl_publisher_create_topic(publisher, empty, &schema, &err), FL_INVALID_ARGUMENT);
    EXPECT_EQ(err.origin, FL_ORIGIN_SEAM);
    EXPECT_NE(MessageOf(err).find("topic:"), std::string::npos)
        << "the refusal did not come from the seam's own validator: " << MessageOf(err);
    fl_error_dispose(&err);

    const fl_str slashed[] = {Str("a/b")};
    const fl_topic with_slash = {slashed, 1};
    EXPECT_EQ(fl_publisher_create_topic(publisher, with_slash, &schema, &err), FL_INVALID_ARGUMENT);
    fl_error_dispose(&err);

    schema.release(&schema);
    fl_publisher_destroy(publisher);
    fl_provider_destroy(provider);
}

// ---------------------------------------------------------------------------
// The publisher chain and the fusion
// ---------------------------------------------------------------------------

/// The chain end to end: provider → publisher → topic → fused publish.
///
/// `list_topics` is what makes this observable without a subscriber (BIND-4's,
/// per D-BIND-31): the topic a publish went to is the topic the publisher says
/// it declared, and a publish to an undeclared topic is refused rather than
/// silently dropped.
TEST(BindingEntryPoints, TheFusedPublishRunsOverTheWholeChain) {
    fl_error err = {};
    fl_provider* provider = nullptr;
    const fl_provider_config config = {};
    ASSERT_EQ(fl_provider_create(Str("inprocess"), &config, &provider, &err), FL_OK)
        << MessageOf(err);

    fl_publisher* publisher = nullptr;
    ASSERT_EQ(fl_publisher_create(provider, &publisher, &err), FL_OK) << MessageOf(err);

    AbiFixture abi;
    ArrowSchema schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*abi.batch().schema(), &schema).ok());

    const fl_str segments[] = {Str("bind"), Str("rows")};
    const fl_topic topic = {segments, 2};
    ASSERT_EQ(fl_publisher_create_topic(publisher, topic, &schema, &err), FL_OK) << MessageOf(err);
    schema.release(&schema);  // deep-copied by the shim, so this is safe now

    fl_string_list* topics = nullptr;
    ASSERT_EQ(fl_publisher_list_topics(publisher, &topics, &err), FL_OK) << MessageOf(err);
    ASSERT_EQ(fl_string_list_size(topics), 1U);
    const fl_str name = fl_string_list_at(topics, 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(name.data), name.len), "bind/rows");
    fl_string_list_dispose(topics);

    // One row, then the whole batch in one crossing.
    EXPECT_EQ(fl_publisher_publish_row(publisher, topic, abi.rows(), 0, nullptr, &err), FL_OK)
        << MessageOf(err);
    EXPECT_EQ(fl_publisher_publish_rows(publisher, topic, abi.rows(), 0, abi.batch().num_rows(),
                                        nullptr, &err),
              FL_OK)
        << MessageOf(err);

    // A row index outside the bound batch is the CODEC's refusal even though the
    // entry point is a seam one — the re-attribution the fusion does from inside
    // the encoder lambda.
    EXPECT_NE(fl_publisher_publish_row(publisher, topic, abi.rows(), 99, nullptr, &err), FL_OK);
    EXPECT_EQ(err.origin, FL_ORIGIN_CODEC)
        << "a codec failure inside the fusion was reported as the seam's";
    fl_error_dispose(&err);

    // An undeclared topic is NOT refused here, and that is the provider's
    // contract rather than an omission: `inprocess` defaults to
    // `schema_carriage=as_declared`, where there is no schema for a sample to be
    // missing. The refusal lives in the `carried` mode, which the next row
    // reaches through the config document.
    const fl_str other[] = {Str("never"), Str("declared")};
    const fl_topic undeclared = {other, 2};
    EXPECT_EQ(fl_publisher_publish_row(publisher, undeclared, abi.rows(), 0, nullptr, &err), FL_OK)
        << MessageOf(err);

    fl_publisher_destroy(publisher);
    fl_provider_destroy(provider);
}

/// The config document crosses VERBATIM and reaches the provider.
///
/// Fletcher parses none of it — explicitly a non-goal (seam §4.2) — so the only
/// way to prove the bytes arrive is to send a document that changes what the
/// provider DOES and then observe the change. `schema_carriage=carried` turns
/// schema-before-data into a refusal, so a publish to an undeclared topic that
/// was FL_OK above becomes FL_TOPIC_NOT_DECLARED here, through the same entry
/// point with the same arguments. A shim that dropped the document, truncated it
/// at a length it invented, or sent it as a C string would leave the provider in
/// its default mode and this row would read FL_OK.
TEST(BindingEntryPoints, TheConfigDocumentReachesTheProviderVerbatim) {
    const std::string document = "schema_carriage=carried";
    fl_provider_config config = {};
    config.document = fl_str{reinterpret_cast<const uint8_t*>(document.data()), document.size()};

    fl_error err = {};
    fl_provider* provider = nullptr;
    ASSERT_EQ(fl_provider_create(Str("inprocess"), &config, &provider, &err), FL_OK)
        << MessageOf(err);

    fl_publisher* publisher = nullptr;
    ASSERT_EQ(fl_publisher_create(provider, &publisher, &err), FL_OK) << MessageOf(err);

    AbiFixture abi;
    const fl_str segments[] = {Str("carried")};
    const fl_topic topic = {segments, 1};
    EXPECT_EQ(fl_publisher_publish_row(publisher, topic, abi.rows(), 0, nullptr, &err),
              FL_TOPIC_NOT_DECLARED)
        << "the document did not reach the provider: " << MessageOf(err);
    fl_error_dispose(&err);

    // And a document the provider cannot read is ITS refusal, with its own
    // message — the shim neither validates the syntax nor invents a number.
    const std::string bad = "schema_carriage=sideways";
    config.document = fl_str{reinterpret_cast<const uint8_t*>(bad.data()), bad.size()};
    fl_provider* refused = nullptr;
    EXPECT_EQ(fl_provider_create(Str("inprocess"), &config, &refused, &err), FL_INVALID_ARGUMENT);
    EXPECT_NE(MessageOf(err).find("schema_carriage"), std::string::npos) << MessageOf(err);
    fl_error_dispose(&err);

    fl_publisher_destroy(publisher);
    fl_provider_destroy(provider);
}

/// A writer that reports 0 bytes fails the publish and is blamed on the CALLER.
///
/// Zero is the only channel a `fl_writer_fn` has for "my own thunk caught an
/// exception" — the signature returns a count, not a status (D-BIND-19 rule 3) —
/// so it has to fail the publish rather than publish an empty row. The binding
/// then rethrows the original managed exception with its own stack, which is why
/// the message here only has to be findable, not descriptive.
TEST(BindingEntryPoints, AWriterReportingZeroBytesFailsThePublish) {
    fl_error err = {};
    fl_provider* provider = nullptr;
    const fl_provider_config config = {};
    ASSERT_EQ(fl_provider_create(Str("inprocess"), &config, &provider, &err), FL_OK)
        << MessageOf(err);

    fl_publisher* publisher = nullptr;
    ASSERT_EQ(fl_publisher_create(provider, &publisher, &err), FL_OK) << MessageOf(err);

    AbiFixture abi;
    ArrowSchema schema = {};
    ASSERT_TRUE(arrow::ExportSchema(*abi.batch().schema(), &schema).ok());
    const fl_str segments[] = {Str("raw")};
    const fl_topic topic = {segments, 1};
    ASSERT_EQ(fl_publisher_create_topic(publisher, topic, &schema, &err), FL_OK) << MessageOf(err);
    schema.release(&schema);

    // The control first: a writer that writes something succeeds, so the row
    // below is about the ZERO and not about the path being broken.
    struct Writers {
        static size_t Four(void*, uint8_t* dst, size_t) {
            std::memset(dst, 0, 4);
            return 4;
        }
        static size_t None(void*, uint8_t*, size_t) { return 0; }
    };
    EXPECT_EQ(fl_publisher_publish_raw(publisher, topic, &Writers::Four, nullptr, 4, nullptr, &err),
              FL_OK)
        << MessageOf(err);

    EXPECT_NE(fl_publisher_publish_raw(publisher, topic, &Writers::None, nullptr, 4, nullptr, &err),
              FL_OK);
    EXPECT_EQ(err.origin, FL_ORIGIN_CALLBACK)
        << "a writer's own failure was reported as the seam's";
    fl_error_dispose(&err);

    fl_publisher_destroy(publisher);
    fl_provider_destroy(provider);
}

// ---------------------------------------------------------------------------
// The registry, and the error type itself
// ---------------------------------------------------------------------------

/// The selector's classification is total and disjoint, and both refusals are
/// worth their own message.
TEST(BindingEntryPoints, TheSelectorIsClassifiedBeforeItIsResolved) {
    fl_error err = {};
    fl_provider* provider = nullptr;
    const fl_provider_config config = {};

    // A NAME nobody registered: the registry's refusal lists what IS available,
    // which is the difference between a five-second fix and an afternoon.
    EXPECT_NE(fl_provider_create(Str("nosuchprovider"), &config, &provider, &err), FL_OK);
    EXPECT_EQ(err.origin, FL_ORIGIN_SEAM);
    EXPECT_NE(MessageOf(err).find("inprocess"), std::string::npos)
        << "the refusal did not name the providers that ARE registered: " << MessageOf(err);
    fl_error_dispose(&err);

    // A PATH: well-formed, and names a driver this build cannot load. The honest
    // answer is FL_NOT_SUPPORTED until PDA-ABI fills the resolver seat.
    EXPECT_EQ(fl_provider_create(Str("./driver.so"), &config, &provider, &err), FL_NOT_SUPPORTED)
        << MessageOf(err);
    fl_error_dispose(&err);

    EXPECT_EQ(fl_provider_create(Str(""), &config, &provider, &err), FL_INVALID_ARGUMENT);
    fl_error_dispose(&err);
}

/// `fl_error_dispose` is safe on a zeroed struct and safe twice — which is what
/// lets every binding put it in a `finally` without a null check first.
TEST(BindingEntryPoints, ErrorDisposeIsSafeOnAZeroedStructAndTwice) {
    fl_error zeroed = {};
    fl_error_dispose(&zeroed);
    fl_error_dispose(&zeroed);
    EXPECT_EQ(zeroed.message, nullptr);

    fl_error real = {};
    fl_provider* provider = nullptr;
    const fl_provider_config config = {};
    ASSERT_NE(fl_provider_create(Str("nosuchprovider"), &config, &provider, &real), FL_OK);
    ASSERT_NE(real.message, nullptr);

    fl_error_dispose(&real);
    EXPECT_EQ(real.message, nullptr);
    EXPECT_EQ(real.message_len, 0U);
    EXPECT_EQ(real.status, FL_OK);
    fl_error_dispose(&real);

    fl_error_dispose(nullptr);  // and on nothing at all
}

/// Closing the codec before unbinding the rows is survivable.
///
/// `binding.h` does not forbid this order, and a binding whose handles are
/// collected by a GC cannot control the order its finalisers run in — so the
/// ordinary mistake must not be a use-after-free. The rows hold a share of the
/// codec, which is why the encode below still has a field plan to work from.
TEST(BindingEntryPoints, ClosingTheCodecBeforeUnbindingTheRowsIsSafe) {
    auto batch = Fixture();
    ArrowSchema schema = {};
    ArrowArray array = {};
    ASSERT_TRUE(arrow::ExportRecordBatch(*batch, &array, &schema).ok());

    fl_error err = {};
    fl_codec* codec = nullptr;
    fl_rows* rows = nullptr;
    ASSERT_EQ(fl_codec_open(&schema, &codec, &err), FL_OK) << MessageOf(err);
    ASSERT_EQ(fl_rows_bind(codec, &array, &rows, &err), FL_OK) << MessageOf(err);

    fl_codec_close(codec);  // the wrong order, on purpose

    VectorWindow sink(64);
    EXPECT_EQ(fl_encode_row(rows, 0, &sink.window, &err), FL_OK) << MessageOf(err);
    EXPECT_EQ(sink.Written(), EncodeLocally(*batch, 0));

    fl_rows_unbind(rows);
    array.release(&array);
    schema.release(&schema);
}

/// On success the callee leaves the error untouched, so a caller that checks the
/// message without checking the status cannot read a stale one.
TEST(BindingEntryPoints, SuccessLeavesTheErrorUntouched) {
    AbiFixture abi;
    VectorWindow sink(64);

    fl_error err = {};
    err.status = 12345;  // a value no entry point could produce
    ASSERT_EQ(fl_encode_row(abi.rows(), 0, &sink.window, &err), FL_OK);
    EXPECT_EQ(err.status, 12345) << "a successful call wrote to the error struct";
    EXPECT_EQ(err.message, nullptr);
}

}  // namespace
