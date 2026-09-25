// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The binding ABI's entry points (BIND-2c): the codec surface, the write-window
// adapters, and the publisher chain that makes the publish fusion real.
//
// BIND-4a completed it: the attachments builder, blob retain/release, shared
// schemas, the subscriber half and the schema-arrival pair are all here, and
// every symbol `binding.h` declares is now defined.
//
// D-BIND-31's rule still holds and is the reason for the ONE apparent exception
// below. An unimplemented entry point is a LINK ERROR here, never a symbol that
// answers FL_NOT_SUPPORTED, because that status reads to a binding author
// exactly like a transport that cannot do the thing. `fl_subscriber_subscribe_
// schema` and its pair are not that case: the header DECLARES them as answering
// FL_NOT_SUPPORTED until the seam grows SubscribeSchema/UnsubscribeSchema
// (D-BIND-29), so the status is the specified answer rather than a stub standing
// in for a missing one.
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
#include <chrono>
#include <cstring>
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

/// The seam's per-topic options from their C form. NULL is the all-empty value,
/// and nothing is validated here: the seam checks every field, with its own
/// messages, one level down (D-BIND-57).
fletcher::TopicOptions ToTopicOptions(const fl_topic_options* options) {
    if (options == nullptr) return {};
    return {ToString(options->profile), options->max_payload_bytes};
}

/// The attachments to publish with. NULL means none — and in 2c it is the only
/// thing a caller can pass, because no exported function constructs an
/// `fl_attachments` until BIND-4's builder (D-BIND-31).
const Attachments& AttachmentsOf(const fl_attachments* atts) {
    static const Attachments kNone;
    return atts == nullptr ? kNone : atts->set;
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

/// A NEW reference to a shared schema, in the ABI's owner-handle form.
///
/// The control block is the blob's, deliberately: `fl_schema` and `fl_blob` are
/// the same shape (an opaque owner plus a borrowed pointer) with the same
/// retain/release idiom, and giving them two control blocks would be two
/// refcounts to get right instead of one. What it keeps differs — here the
/// `SharedSchema` itself, which is a `shared_ptr<const ArrowSchema>`, so holding
/// the block holds the schema and nothing deep-copies.
///
/// A NULL schema is the schema-less transport's answer (seam §7 clause 1) and
/// gets no block: there is nothing to keep alive, and the header says that
/// `owner` is NULL exactly then and must not be released.
fl_schema ShareSchema(const fletcher::SharedSchema& schema) {
    if (schema == nullptr) return fl_schema{nullptr, nullptr};
    auto* block = new fletcher::abi::BlobOwner();
    block->keep = schema;
    return fl_schema{block, schema.get()};
}

/// A shared schema for exactly the length of a scope, for the delivery thunk.
///
/// The thunk hands the handler a BORROWED schema and must drop its own reference
/// however it leaves — including when building the attachments view throws,
/// which the seam's fan-out absorbs rather than propagates. A raw
/// make-call-release would leak the block on that path, and the leak would be a
/// schema held forever with nothing pointing at it.
class SchemaScope {
   public:
    explicit SchemaScope(const fletcher::SharedSchema& schema) : view_(ShareSchema(schema)) {}
    ~SchemaScope() { fl_schema_release(&view_); }

    SchemaScope(const SchemaScope&) = delete;
    SchemaScope& operator=(const SchemaScope&) = delete;

    [[nodiscard]] const fl_schema* get() const noexcept { return &view_; }

   private:
    fl_schema view_;
};

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

/* ══ Blobs: shared ownership in C (BIND-4a) ═══════════════════════════════ */

/// Make a blob Fletcher owns, by COPYING the caller's bytes (D-BIND-42).
///
/// The copy is the feature. Rule 1 forbids a view-only blob precisely so that
/// every blob crossing this boundary has an owner whose lifetime is not the
/// caller's problem, and the only way to honour that for bytes a binding already
/// holds is to take our own copy of them. Attachments are sidecar metadata; the
/// row payload's zero-copy path is `fl_publisher_publish_row` and is untouched.
fl_status fl_blob_create(const uint8_t* data, size_t size, fl_blob* out, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_blob_create");
        if (data == nullptr && size != 0) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_blob_create: a null data pointer cannot carry " +
                                  std::to_string(size) + " bytes");
        }

        // Rule 5: an empty blob needs no owner, so it is not worth a control
        // block and must report {NULL, NULL, 0} whatever it was built from.
        if (size == 0) {
            *out = fl_blob{nullptr, nullptr, 0};
            return;
        }

        auto* block = new fletcher::abi::BlobOwner();
        auto bytes = std::make_shared<const std::vector<uint8_t>>(data, data + size);
        block->keep = bytes;
        *out = fl_blob{block, bytes->data(), bytes->size()};
    });
}

/// Retain and release are the ONLY operations on `owner`, and both are no-ops on
/// an empty blob — the header's rule 5 says an empty blob has no owner because
/// there is no byte to keep alive, so there is nothing to count.
///
/// Neither re-enters the seam and neither can fail, which is why they return
/// void and take no `fl_error`: a release that could fail would leave a binding's
/// finaliser with nowhere to put the failure.
void fl_blob_retain(const fl_blob* blob) {
    if (blob == nullptr || blob->owner == nullptr) return;
    auto* block = static_cast<fletcher::abi::BlobOwner*>(blob->owner);
    // Relaxed is enough to ADD a reference: the caller already holds one, so the
    // object cannot die under us and no other memory is being published.
    block->refs.fetch_add(1, std::memory_order_relaxed);
}

void fl_blob_release(const fl_blob* blob) {
    if (blob == nullptr || blob->owner == nullptr) return;
    auto* block = static_cast<fletcher::abi::BlobOwner*>(blob->owner);
    // acq_rel on the way down, unlike the add: the thread that drops the last
    // reference must see every write the other holders made before it runs the
    // destructor.
    if (block->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete block;
}

/* ══ Shared schemas ═══════════════════════════════════════════════════════ */

/// Drop a reference to a shared schema.
///
/// The header's most expensive warning lives on this type: a binding must NEVER
/// call the Arrow C Data Interface's own `release` on `fl_schema::schema`,
/// because that destroys the schema for every other holder — including the
/// provider still delivering on it. This is the sanctioned way to let go, and
/// like the blob pair it never fails and never re-enters the seam.
/// Take a reference (D-BIND-43). The blob pair's reasoning applies unchanged,
/// including the orderings: relaxed to add, because the caller already holds a
/// reference and nothing else is being published.
void fl_schema_retain(const fl_schema* schema) {
    if (schema == nullptr || schema->owner == nullptr) return;
    auto* block = static_cast<fletcher::abi::BlobOwner*>(schema->owner);
    block->refs.fetch_add(1, std::memory_order_relaxed);
}

void fl_schema_release(const fl_schema* schema) {
    if (schema == nullptr || schema->owner == nullptr) return;
    auto* block = static_cast<fletcher::abi::BlobOwner*>(schema->owner);
    if (block->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete block;
}

/// Deep-copy a shared schema into a structure the caller owns (D-BIND-46).
///
/// THE SURFACE COULD NOT DO THIS AND ITS OWN DOCUMENTATION SAID IT COULD: the
/// `fl_schema` comment tells a binding to import "a deep copy the shim provides",
/// and until this call nothing provided one. Found the same way `fl_blob_create`
/// and `fl_schema_retain` were - by writing the managed code that had to consume
/// the thing - and it is the same root cause all three share: an owner-handle
/// type whose OUTBOUND direction had no consumer until now.
///
/// The copy is INDEPENDENT. `OwnedSchema::DeepCopy` is the seam's own, already
/// used inbound by `fl_publisher_create_topic`, so the two directions cannot
/// disagree about what a deep copy is.
fl_status fl_schema_copy(const fl_schema* schema, struct ArrowSchema* out, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_schema_copy");
        if (schema == nullptr || schema->schema == nullptr) {
            // A schema-less transport answers kOk with a NULL schema, which is an
            // ANSWER (seam §7 clause 1) and not something to copy from. A caller
            // here has skipped reading it.
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_schema_copy: there is no schema to copy; a NULL schema is the "
                              "schema-less transport's answer, not a failure");
        }

        OwnedSchema owned = OwnedSchema::DeepCopy(schema->schema);

        // Hand the structure over by MOVE, which is what `OwnedSchema`'s own move
        // constructor does: copy the struct (release callback included) and zero
        // the source so its destructor does not release what the caller now owns.
        // Doing it any other way would either double-release or leak.
        *out = *owned.get();
        std::memset(owned.get(), 0, sizeof(ArrowSchema));
    });
}

/* ══ Attachments: the read end ════════════════════════════════════════════ */

size_t fl_attachments_size(const fl_attachments* atts) {
    return atts == nullptr ? 0 : atts->set.size();
}

/// Out of range yields {NULL, 0} rather than a status, exactly as
/// `fl_string_list_at` does: the signature has nowhere to put one, and the caller
/// learns the size from the call above.
fl_str fl_attachments_key_at(const fl_attachments* atts, size_t index) {
    if (atts == nullptr || index >= atts->set.size()) return fl_str{nullptr, 0};
    const std::string_view key = atts->set.KeyAt(index);
    return fl_str{reinterpret_cast<const uint8_t*>(key.data()), key.size()};
}

/// BORROWED from the set: the control block is the set's, built at seal time, and
/// this hands back a reference it does NOT add to. A caller keeping the bytes
/// past the set calls `fl_blob_retain` — the header's rule 1, and the reason this
/// does not retain on the caller's behalf.
fl_blob fl_attachments_value_at(const fl_attachments* atts, size_t index) {
    if (atts == nullptr || index >= atts->set.size()) return fl_blob{nullptr, nullptr, 0};
    const fletcher::Blob& value = atts->set.ValueAt(index);
    if (value.empty()) return fl_blob{nullptr, nullptr, 0};
    return fl_blob{atts->owners[index], value.data(), value.size()};
}

/// Absence is not a failure and carries no error, which is why this returns an
/// int rather than an `fl_status`: "no such key" is an ordinary answer, and a
/// caller forced to distinguish it from a refusal would need a taxonomy for
/// something that is not one.
int fl_attachments_find(const fl_attachments* atts, fl_str key, fl_blob* out) {
    if (atts == nullptr || out == nullptr) return 0;
    const std::string_view wanted(reinterpret_cast<const char*>(key.data), key.len);
    for (size_t i = 0; i < atts->set.size(); ++i) {
        if (atts->set.KeyAt(i) != wanted) continue;
        *out = fl_attachments_value_at(atts, i);
        return 1;
    }
    return 0;
}

void fl_attachments_dispose(fl_attachments* atts) { delete atts; }

/* ══ Attachments: the write end ═══════════════════════════════════════════ */

fl_status fl_attachments_builder_create(fl_attachments_builder** out, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_attachments_builder_create");
        *out = new fl_attachments_builder();
    });
}

/// `key` is COPIED and `value` is RETAINED, so the caller may release its own
/// reference the moment this returns. A key containing a zero byte is refused —
/// the seam's own rule, inherited rather than re-implemented, because
/// `Attachments::Set` raises it.
fl_status fl_attachments_builder_set(fl_attachments_builder* builder, fl_str key,
                                     const fl_blob* value, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (builder == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_attachments_builder_set: builder must not be null");
        }

        std::string copied(reinterpret_cast<const char*>(key.data), key.len);

        if (value == nullptr || value->size == 0) {
            builder->pending.Set(std::move(copied), fletcher::Blob());
            return;
        }

        // Retaining means holding the CALLER's control block, not copying the
        // bytes: that is what makes "value is RETAINED by the builder" true
        // without a copy, and what lets a transport's loaned sample cross here.
        auto* block = static_cast<fletcher::abi::BlobOwner*>(value->owner);
        if (block == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_attachments_builder_set: a blob with bytes needs an owner that "
                              "keeps them alive; there is no view-only blob");
        }
        block->refs.fetch_add(1, std::memory_order_relaxed);

        // The deleter drops the reference just taken, so the retained block dies
        // with the Blob rather than with this call.
        std::shared_ptr<const void> keep(static_cast<const void*>(block), [block](const void*) {
            if (block->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete block;
        });
        builder->pending.Set(std::move(copied),
                             fletcher::Blob(std::move(keep), value->data, value->size));
    });
}

/// Seal. The builder is left EMPTY and reusable, which is the header's wording
/// and is what lets a publisher build one set per row without reallocating.
fl_status fl_attachments_builder_build(fl_attachments_builder* builder, fl_attachments** out,
                                       fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_attachments_builder_build");
        if (builder == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_attachments_builder_build: builder must not be null");
        }
        *out = new fl_attachments(std::move(builder->pending));
        builder->pending = fletcher::Attachments();
    });
}

void fl_attachments_builder_dispose(fl_attachments_builder* builder) { delete builder; }

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
    return fl_publisher_create_topic_with_options(publisher, topic, schema, nullptr, err);
}

/// Both forms, one body. The seam's own two-argument `CreateTopic` is the
/// empty-options case of the three-argument one, so this forwards to the latter
/// unconditionally - an empty value reaches a provider that never heard of
/// options as its ordinary call (D-BIND-57).
fl_status fl_publisher_create_topic_with_options(fl_publisher* publisher, fl_topic topic,
                                                 const struct ArrowSchema* schema,
                                                 const fl_topic_options* options, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (publisher == nullptr || schema == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_publisher_create_topic: publisher and schema must not be null");
        }
        // Deep-copied here, so a binding may release its Arrow export as soon as
        // this returns. N-5: the C Data Interface's `release` and the seam's
        // owner-handle protocol are two different lifetimes, and this is the one
        // place they meet.
        publisher->publisher->CreateTopic(ToSegments(topic), OwnedSchema::DeepCopy(schema),
                                          ToTopicOptions(options));
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

/* ══ Subscriber ═════════════════════════════════════════════════════════════════════════ */

fl_status fl_subscriber_create(fl_provider* provider, fl_subscriber** out, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_subscriber_create");
        if (provider == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_subscriber_create: provider must not be null");
        }
        *out = new fl_subscriber(provider->provider);
    });
}

/// Destroy a subscriber, and NOTE WHAT THIS DOES NOT DO.
///
/// It does not check for quiescence and it cannot: from inside a delivery on
/// this same subscriber the seam's `~Subscriber` reaches the provider's door,
/// is refused with kReentrantCall, and rethrows out of a `noexcept` destructor -
/// the program stops, by design, because the alternative is a silently leaked
/// transport subscription. That answer is the SEAM's and the shim must not
/// soften it: a `catch` here would turn a designed halt into a leak with no
/// signal at all.
///
/// The refusal a caller can actually act on belongs one level up, in the
/// binding's own code, before the call can reach this symbol (D-BIND-18).
void fl_subscriber_destroy(fl_subscriber* subscriber) { delete subscriber; }

fl_status fl_subscriber_subscribe(fl_subscriber* subscriber, fl_topic topic,
                                  fl_delivery_fn on_delivery, void* ctx, uint64_t* out_id,
                                  fl_schema_arrival** out_arrival, fl_error* err) {
    return fl_subscriber_subscribe_with_options(subscriber, topic, on_delivery, ctx, nullptr,
                                                out_id, out_arrival, err);
}

/// Both forms, one body, for the reason fl_publisher_create_topic_with_options
/// gives (D-BIND-57).
fl_status fl_subscriber_subscribe_with_options(fl_subscriber* subscriber, fl_topic topic,
                                               fl_delivery_fn on_delivery, void* ctx,
                                               const fl_topic_options* options, uint64_t* out_id,
                                               fl_schema_arrival** out_arrival, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out_id, "fl_subscriber_subscribe (out_id)");
        RequireOut(out_arrival, "fl_subscriber_subscribe (out_arrival)");
        if (subscriber == nullptr || on_delivery == nullptr) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "fl_subscriber_subscribe: subscriber and on_delivery must not be null");
        }

        // Allocated BEFORE the subscription goes live, and the ordering is the
        // point rather than a style choice. Between `Subscribe` returning and
        // the caller learning its id there must be nothing that can throw: a
        // failure in that gap leaves a subscription running, delivering into a
        // thunk, with no id anywhere for anyone to cancel it by. Everything
        // after the call below is a move-assign of a `shared_ptr`, a pointer
        // store and a `release()` - all noexcept.
        auto arrival = std::make_unique<fl_schema_arrival>(fletcher::SchemaArrival{});

        auto result = subscriber->subscriber->Subscribe(
            ToSegments(topic),
            [on_delivery, ctx](uint64_t id, const uint8_t* data, size_t len,
                               const fletcher::SharedSchema& schema, const Attachments& atts) {
                // THE THUNK. Both views are borrowed for exactly this call,
                // which is what the header promises a handler, and both are torn
                // down however this frame exits - `SchemaScope`'s destructor and
                // `view`'s. A handler that wants either past the return retains
                // it (fl_schema_retain, fl_blob_retain) and the block outlives
                // the scope that made it.
                //
                // `on_delivery` is a C function pointer and cannot throw; a
                // binding's own thunk is what catches the handler's exceptions
                // (seam 5.3). What CAN throw here is building the views, and the
                // seam's fan-out absorbs that and counts it in
                // `AbsorbedCallbackFailures()` rather than letting it reach a
                // transport thread's C frames.
                const SchemaScope shared(schema);

                // A COPY of the set per delivery, and the cost is stated rather
                // than hidden: one control block per entry, built here because
                // `fl_attachments` builds them at seal time so that a blob it
                // hands out is retainable. A delivery with no attachments
                // allocates nothing. Whether this is worth a borrowing form is a
                // question for D-BIND-37's benchmark, not for a guess here.
                fl_attachments view(atts);

                on_delivery(ctx, id, data, len, shared.get(), &view);
            },
            ToTopicOptions(options));

        arrival->arrival = std::move(result.schema);
        *out_id = result.subscription_id;
        *out_arrival = arrival.release();
    });
}

/// Cancel a subscription.
///
/// Nothing is translated here, including the two surprises, because both are the
/// seam's contract and a shim that smoothed either would be describing a
/// different system: cancelling something that is not live is a NO-OP rather
/// than an error, and a cancellation issued from inside a delivery does not wait
/// for the frame it is already in.
fl_status fl_subscriber_unsubscribe(fl_subscriber* subscriber, uint64_t subscription_id,
                                    fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (subscriber == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_subscriber_unsubscribe: subscriber must not be null");
        }
        subscriber->subscriber->Unsubscribe(subscription_id);
    });
}

/// The schema watch (D-BIND-52): the seam's `SubscribeSchema`, forwarded.
///
/// Every rule the header states is the SEAM's, not this file's - the per-subscriber
/// count, the idempotence per topic, the refusal from inside a delivery, the
/// `kNotSupported` from a transport with no schema channel - so nothing here
/// re-implements one. What this file owns is the handle and the order: the arrival
/// is allocated BEFORE the watch is registered, for the reason
/// `fl_subscriber_subscribe` gives, so that once the seam has counted a watch
/// nothing left can throw and strand it without a handle to answer through.
///
/// Until D-BIND-52 this answered FL_NOT_SUPPORTED with a message saying the seam
/// had no such pair (D-BIND-29) - true when written, false once #128 reached this
/// branch.
fl_status fl_subscriber_subscribe_schema(fl_subscriber* subscriber, fl_topic topic,
                                         fl_schema_arrival** out_arrival, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out_arrival, "fl_subscriber_subscribe_schema");
        if (subscriber == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_subscriber_subscribe_schema: subscriber must not be null");
        }

        auto arrival = std::make_unique<fl_schema_arrival>(fletcher::SchemaArrival{});
        arrival->arrival = subscriber->subscriber->SubscribeSchema(ToSegments(topic));
        *out_arrival = arrival.release();
    });
}

/// Releases one watch. A no-op for a topic this subscriber does not watch - the
/// seam's rule, so teardown may call it unconditionally.
fl_status fl_subscriber_unsubscribe_schema(fl_subscriber* subscriber, fl_topic topic,
                                           fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (subscriber == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_subscriber_unsubscribe_schema: subscriber must not be null");
        }
        subscriber->subscriber->UnsubscribeSchema(ToSegments(topic));
    });
}

/* ══ The schema arrival ════════════════════════════════════════════════════════════ */

/// Wait, and report FIVE outcomes across TWO channels.
///
/// Three of them are values and are RETURNED (FL_OK, FL_PENDING,
/// FL_SUBSCRIPTION_ENDED); a genuine failure is THROWN, so that the one
/// containment site fills `err` with the number and the message exactly as it
/// does for every other entry point. That split is why this function cannot just
/// be a `Contain` call whose status is the answer: `Contain` reports FL_OK for
/// anything that did not throw, and "not yet" is not a failure.
///
/// The timeout is FORWARDED, not interpreted. A negative is refused by
/// `SchemaArrival::Wait` itself with kInvalidArgument, and INT64_MAX is
/// `milliseconds::max()` exactly - same underlying type, same value - so the
/// unbounded form arrives as the unbounded form without this boundary inventing
/// a mapping. D-BIND-20's "negative means forever" belongs to C#, above this
/// call, and must not be invented here.
fl_status fl_schema_arrival_wait(fl_schema_arrival* arrival, int64_t timeout_ms, fl_schema* out,
                                 fl_error* err) {
    fl_status outcome = FL_OK;
    const fl_status contained = Contain(err, FL_ORIGIN_SEAM, [&] {
        RequireOut(out, "fl_schema_arrival_wait");
        if (arrival == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_schema_arrival_wait: arrival must not be null");
        }

        fletcher::SharedSchema shared;
        const PubSubStatus status =
            arrival->arrival.Wait(std::chrono::milliseconds(timeout_ms), &shared);

        switch (status) {
            case PubSubStatus::kOk:
                // Written on BOTH kOk shapes. A schema-less transport answers
                // kOk with a null schema, and `ShareSchema` turns that into
                // {NULL, NULL} - the header is explicit that `*out` is written
                // then, because a caller left reading whatever it passed in is
                // the silent wrong-slot decode clause 7 exists to prevent.
                *out = ShareSchema(shared);
                outcome = FL_OK;
                return;
            case PubSubStatus::kPending:
                outcome = FL_PENDING;
                return;
            case PubSubStatus::kSubscriptionEnded:
                outcome = FL_SUBSCRIPTION_ENDED;
                return;
            default:
                // A real failure. `PubSubError` refuses to carry the three
                // statuses above, which is why they are returned before this
                // line can see them.
                throw PubSubError(status, arrival->arrival.Message());
        }
    });
    return contained == FL_OK ? outcome : contained;
}

/// Dispose the handle, and nothing else. The subscription behind it is untouched
/// - this holds a COPY of a copyable arrival - and a schema already handed out by
/// `wait` keeps the reference it was given.
void fl_schema_arrival_dispose(fl_schema_arrival* arrival) { delete arrival; }

}  // extern "C"
