// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The binding ABI's entry points (BIND-2c): the codec surface, the write-window
// adapters, and the publisher chain that makes the publish fusion real.
//
// Scope is D-BIND-31's: the subscriber half, the attachments builder, blob
// retain/release and the schema-arrival pair are BIND-4's and are deliberately
// absent here rather than stubbed — an exported symbol that answers
// FL_NOT_SUPPORTED reads to a binding author exactly like a transport that
// cannot do the thing, which is a different and much more confusing statement
// than "not linked yet".
//
// ── The one rule every function in this file obeys ──────────────────────────
// A C++ exception unwinding through a C function is undefined behaviour, so
// every body below is wrapped in `Contain`, which is the single containment
// site (containment.hpp). No function here has a `try` of its own, no function
// here invents a status number, and no function here formats an error message
// for a failure the layer below already described. Where this file looks like it
// is choosing a taxonomy it is forwarding one.
//
// ── What is NOT re-validated here, and why that is the point ────────────────
// The seam refuses six things about a topic (an empty list, a zero byte, a '/',
// an empty segment, a leading "__", a joined name above 246 bytes) inside
// `RequireSegments`, which `JoinSegments` calls on EVERY path a publisher takes.
// The shim therefore inherits all six rather than keeping a second copy of them.
// That is the same structural choice decode made against `PositionalReader` in
// BIND-2b, for the same reason: two copies of a taxonomy are two things free to
// drift, and the one that drifts silently is the one nobody is testing.
//
// `binding.h` says "the shim re-validates". It does, in the only sense that
// survives review — every call goes through the code that refuses — but not by
// restating the rules here.
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <string>
#include <vector>

#include "containment.hpp"
#include "handles.hpp"
#include "single_copy.hpp"
#include "write_window.hpp"

namespace fletcher::abi::internal {
/// Defined in builtins.cpp: the shim's own registry, with `inprocess`, `fastdds`
/// and `xrce` registered into it.
ProviderRegistry& BuiltinRegistry();
}  // namespace fletcher::abi::internal

namespace {

using fletcher::Attachments;
using fletcher::OwnedSchema;
using fletcher::ProviderConfig;
using fletcher::ProviderSelector;
using fletcher::PubSubError;
using fletcher::PubSubStatus;
using fletcher::abi::Contain;
using fletcher::abi::WindowBuffer;
using fletcher::abi::WriteThroughCaller;

/// An `fl_str` as a `std::string`. The LENGTH is authoritative and a zero byte
/// inside the bytes is carried, not truncated (seam §3.5) — which is the whole
/// reason the ABI does not use C strings.
std::string ToString(fl_str s) {
    if (s.data == nullptr || s.len == 0) return {};
    return {reinterpret_cast<const char*>(s.data), s.len};
}

/// An `fl_topic` as the seam's segment vector.
///
/// Nothing is validated here on purpose (see the file header): a malformed topic
/// is refused by `RequireSegments` with its own message, one level down, and
/// arrives at the caller through the containment site unchanged.
std::vector<std::string> ToSegments(fl_topic topic) {
    std::vector<std::string> segments;
    segments.reserve(topic.count);
    for (size_t i = 0; i < topic.count; ++i) {
        segments.push_back(ToString(topic.segments[i]));
    }
    return segments;
}

/// The attachments to publish with. NULL means none — and in 2c it is the only
/// thing a caller can pass, because no exported function constructs an
/// `fl_attachments` until BIND-4's builder (D-BIND-31).
const Attachments& AttachmentsOf(const fl_attachments* atts) {
    static const Attachments kNone;
    return atts == nullptr ? kNone : atts->value;
}

/// Refuse a null OUT parameter before anything is built.
///
/// Every creating entry point needs this and none of them can report it any
/// other way: with nowhere to put the handle, succeeding would leak the thing
/// that was made.
void RequireOut(const void* out, const char* what) {
    if (out == nullptr) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          std::string(what) + ": the out parameter must not be null");
    }
}

}  // namespace

namespace {

/// The load-time single-copy check (D-BIND-17).
///
/// A dynamic initializer, because there is no other hook a shared library gets
/// on both platforms that runs before a caller can reach an entry point. It
/// latches a verdict and never throws; the refusal itself is served by the
/// containment site, so a poisoned shim answers every fallible call with the
/// same message instead of failing to load and taking the host down with no
/// diagnostic a managed runtime could surface.
const bool kSingleCopyChecked = (fletcher::abi::CheckSingleCopy(), true);

}  // namespace

extern "C" {

/* ══ The single-copy marker ════════════════════════════════════════════════ */

const char* fl_single_copy_marker(void) {
    // Touches the load-time check's flag so no linker can decide the
    // initializer above is unreachable and drop it - the same dead-stripping
    // problem BIND-0's builtins touch existed to solve, and MSVC's /OPT:REF in
    // a Release link is the one that bites.
    (void)kSingleCopyChecked;
    return fletcher::abi::MarkerText();
}

/* ══ Errors ════════════════════════════════════════════════════════════════ */

void fl_error_dispose(fl_error* err) {
    if (err == nullptr) return;
    // `delete[]` pairs with the `new[]` in containment.cpp's SetMessage, and the
    // two must keep pairing. It lives HERE rather than beside that allocation
    // because it is an exported entry point and every exported entry point is in
    // this file - which is also what lets the containment logic sit in the object
    // library where the tests can reach it.
    delete[] err->message;
    err->status = static_cast<int32_t>(FL_OK);
    err->origin = static_cast<int32_t>(FL_ORIGIN_NONE);
    err->message = nullptr;
    err->message_len = 0;
}

/* ══ Owned string lists ════════════════════════════════════════════════════ */

size_t fl_string_list_size(const fl_string_list* list) {
    return list == nullptr ? 0 : list->items.size();
}

fl_str fl_string_list_at(const fl_string_list* list, size_t index) {
    // Out of range is an empty string rather than a status: the signature has
    // nowhere to put one, and the caller learns the size from the call above.
    if (list == nullptr || index >= list->items.size()) return fl_str{nullptr, 0};
    const std::string& item = list->items[index];
    return fl_str{reinterpret_cast<const uint8_t*>(item.data()), item.size()};
}

void fl_string_list_dispose(fl_string_list* list) { delete list; }

/* ══ The codec ═════════════════════════════════════════════════════════════ */

fl_status fl_codec_open(const struct ArrowSchema* schema, fl_codec** out, fl_error* err) {
    return Contain(err, FL_ORIGIN_CODEC, [&] {
        RequireOut(out, "fl_codec_open");
        if (schema == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_codec_open: schema must not be null");
        }
        // The codec deep-copies, so the caller may release its export the moment
        // this returns — which is what lets a binding open a codec from a schema
        // it borrowed for one call.
        *out = new fl_codec(*schema);
    });
}

void fl_codec_close(fl_codec* codec) { delete codec; }

fl_status fl_rows_bind(const fl_codec* codec, const struct ArrowArray* array, fl_rows** out,
                       fl_error* err) {
    return Contain(err, FL_ORIGIN_CODEC, [&] {
        RequireOut(out, "fl_rows_bind");
        if (codec == nullptr || array == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_rows_bind: codec and array must not be null");
        }
        // Every buffer of every child is validated HERE, once per batch, by
        // `ArrowArrayViewSetArray` inside BoundRows. The per-row publish path
        // below therefore validates nothing and allocates nothing.
        *out = new fl_rows(codec->codec, *array);
    });
}

void fl_rows_unbind(fl_rows* rows) {
    // Releases the VIEW only. The caller's array is borrowed and stays the
    // caller's to release — one export serves N publishes, and consuming it here
    // is the mistake `BoundRows` exists to make impossible.
    delete rows;
}

fl_status fl_encode_row(const fl_rows* rows, int64_t i, fl_write_window* sink, fl_error* err) {
    fl_origin origin = FL_ORIGIN_CODEC;
    return Contain(err, &origin, [&] {
        if (rows == nullptr || sink == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_encode_row: rows and sink must not be null");
        }
        WindowBuffer buffer(sink, &origin);
        rows->codec->EncodeRow(*rows->rows, i, buffer);
        buffer.Commit();
    });
}

fl_status fl_decode_rows(const fl_codec* codec, const uint8_t* bytes, size_t len, int64_t count,
                         struct ArrowArray* out, fl_error* err) {
    return Contain(err, FL_ORIGIN_CODEC, [&] {
        if (codec == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_decode_rows: codec must not be null");
        }
        // A pass-through, deliberately. Every refusal below this line is the
        // positional reader's, which is what makes BIND-2b's parity property
        // ("every refusal carries the reader's prefix") true of the ABI and not
        // only of the codec.
        codec->codec->DecodeRows(bytes, len, count, out);
    });
}

/* ══ Provider ══════════════════════════════════════════════════════════════ */

fl_status fl_provider_create(fl_str selector, const fl_provider_config* config, fl_provider** out,
                             fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_provider_create");
        if (config == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_provider_create: config must not be null");
        }

        ProviderConfig cfg;
        cfg.max_payload_bytes = config->max_payload_bytes;
        cfg.domain_id = config->domain_id;
        // Copied verbatim and never parsed — explicitly a non-goal (seam §4.2).
        // The length is authoritative, so a document with a zero byte in it
        // survives the crossing whole.
        cfg.document = ToString(config->document);

        // `Parse` classifies; `Create` resolves. A PATH selector reaches the
        // resolver seat PDA-ABI has not filled yet and is refused there with the
        // registry's own message, which names what IS available — a refusal
        // worth forwarding rather than replacing.
        const ProviderSelector parsed = ProviderSelector::Parse(ToString(selector));
        *out = new fl_provider(fletcher::abi::internal::BuiltinRegistry().Create(parsed, cfg));
    });
}

void fl_provider_destroy(fl_provider* provider) { delete provider; }

/* ══ Publisher ═════════════════════════════════════════════════════════════ */

fl_status fl_publisher_create(fl_provider* provider, fl_publisher** out, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_publisher_create");
        if (provider == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_create: provider must not be null");
        }
        *out = new fl_publisher(provider->provider);
    });
}

void fl_publisher_destroy(fl_publisher* publisher) { delete publisher; }

fl_status fl_publisher_create_topic(fl_publisher* publisher, fl_topic topic,
                                    const struct ArrowSchema* schema, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (publisher == nullptr || schema == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_create_topic: publisher and schema must not be null");
        }
        // Deep-copied here, so a binding may release its Arrow export as soon as
        // this returns. N-5: the C Data Interface's `release` and the seam's
        // owner-handle protocol are two different lifetimes, and this is the one
        // place they meet.
        publisher->publisher->CreateTopic(ToSegments(topic), OwnedSchema::DeepCopy(schema));
    });
}

fl_status fl_publisher_list_topics(const fl_publisher* publisher, fl_string_list** out,
                                   fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_publisher_list_topics");
        if (publisher == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_list_topics: publisher must not be null");
        }
        *out = new fl_string_list(publisher->publisher->ListTopics());
    });
}

fl_status fl_publisher_publish_raw(fl_publisher* publisher, fl_topic topic, fl_writer_fn writer,
                                   void* ctx, size_t min_bytes, const fl_attachments* atts,
                                   fl_error* err) {
    fl_origin origin = FL_ORIGIN_SEAM;
    return Contain(err, &origin, [&] {
        if (publisher == nullptr || writer == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_publish_raw: publisher and writer must not be null");
        }
        publisher->publisher->Publish(
            ToSegments(topic),
            [&](fletcher::WriteBuffer& out) {
                WriteThroughCaller(out, writer, ctx, min_bytes, &origin);
            },
            AttachmentsOf(atts));
    });
}

/* ══ The publish fusion ════════════════════════════════════════════════════ */

/* The whole point of the ABI, in eleven lines.
 *
 * `Publish` hands the encoder the PROVIDER'S OWN window, so `EncodeRow` writes
 * the row's bytes where the transport will read them and no intermediate buffer
 * ever holds the row. That is what makes the copy oracle score `encode_copies ==
 * 0` for a foreign-language producer (2d), and it is why no encode entry point
 * in this header returns bytes: a function that hands the row back has already
 * put it somewhere other than the transport. */
fl_status fl_publisher_publish_row(fl_publisher* publisher, fl_topic topic, const fl_rows* rows,
                                   int64_t i, const fl_attachments* atts, fl_error* err) {
    fl_origin origin = FL_ORIGIN_SEAM;
    return Contain(err, &origin, [&] {
        if (publisher == nullptr || rows == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_publish_row: publisher and rows must not be null");
        }
        publisher->publisher->Publish(
            ToSegments(topic),
            [&](fletcher::WriteBuffer& out) {
                // Re-attributed so a malformed row is reported as the CODEC's
                // failure even though the entry point is a seam one. The flag is
                // read by the containment site at catch time, so it survives the
                // provider re-throwing on the way out.
                origin = FL_ORIGIN_CODEC;
                rows->codec->EncodeRow(*rows->rows, i, out);
                origin = FL_ORIGIN_SEAM;
            },
            AttachmentsOf(atts));
    });
}

fl_status fl_publisher_publish_rows(fl_publisher* publisher, fl_topic topic, const fl_rows* rows,
                                    int64_t first, int64_t count,
                                    const fl_attachments* const* atts_per_row, fl_error* err) {
    fl_origin origin = FL_ORIGIN_SEAM;
    return Contain(err, &origin, [&] {
        if (publisher == nullptr || rows == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_publish_rows: publisher and rows must not be null");
        }
        if (count < 0) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "fl_publisher_publish_rows: count must be >= 0, got " + std::to_string(count));
        }

        // N samples, ONE crossing. The segments are converted once rather than
        // per row, which is the only reason this is not just a loop the caller
        // could have written — and it is why the signature takes a range.
        const std::vector<std::string> segments = ToSegments(topic);
        for (int64_t n = 0; n < count; ++n) {
            const fl_attachments* atts = atts_per_row == nullptr ? nullptr : atts_per_row[n];
            publisher->publisher->Publish(
                segments,
                [&](fletcher::WriteBuffer& out) {
                    origin = FL_ORIGIN_CODEC;
                    rows->codec->EncodeRow(*rows->rows, first + n, out);
                    origin = FL_ORIGIN_SEAM;
                },
                AttachmentsOf(atts));
        }
        // Partial publication is NOT unwound, and cannot be: rows already handed
        // to the transport have gone out. A failure at row k means rows
        // [first, first + k) were published and the rest were not, which the
        // caller learns from the status and must treat as a resend decision
        // rather than a rollback.
    });
}

}  // extern "C"
