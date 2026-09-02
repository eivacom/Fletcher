// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/in_process_provider.hpp"

#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fletcher/pubsub/internal/schema_conflict.hpp"
#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {

namespace {

// Which of §7 clause 1's two schema modes an instance is in. No longer public
// (PDA-DEC-5): the only route to it is the `schema_carriage` document key, so
// an object with an undecided mode cannot exist and a caller cannot pick the
// mode any other way.
enum class SchemaCarriage {
    // **What the loopback has always done, and the default** — so the gateway
    // is unchanged and keeps sending `schemaIpc`. The topic carries whatever
    // schema a publisher declared **on this instance**, and null for a topic
    // nobody declared.
    kAsDeclared,
    // Schema-before-data, the mode a schema-carrying transport is in. Every
    // delivery carries a non-null schema, upheld by refusal rather than by
    // buffering: `CreateTopic` requires a real schema and `Publish` to an
    // undeclared topic is kTopicNotDeclared.
    kCarried,
};

// Quote one document entry for a refusal message. Deliberately not the shared
// `Quoted` helper in provider_registry.cpp (decision 8: no shared parser, no
// dependency between a provider TU and the registry TU) — this is a handful of
// lines over one key, not a format.
std::string QuoteEntry(const std::string& entry) {
    std::ostringstream out;
    out << '"' << entry << '"';
    return out.str();
}

// The loopback's own document reader (spec §4.2; owner ruling 2026-09-02: the
// document is the provider's own format, and only the provider reads it).
// Fletcher's seam still reads nothing — this function is reachable only from
// this provider's own constructor, is unshared, and depends on nothing beyond
// <string>/<sstream>.
//
// Format: a sequence of `\n`-separated `key=value` entries. Empty document ->
// the defaults. A trailing `\r` on an entry is stripped (H2: a document
// written on Windows is CRLF, and the same text must mean the same thing on
// every platform); nothing else is trimmed, no case folding, no comments. An
// empty entry (`\n\n`, a trailing newline) is skipped. Anything else — an
// unknown key, an unknown value, an entry with no `=`, or a duplicate key — is
// refused with `PubSubError(kInvalidArgument)` quoting the offending entry, so
// a misconfigured loopback never exists (rung-2 case 6).
SchemaCarriage ParseSchemaCarriage(const std::string& document) {
    SchemaCarriage carriage = SchemaCarriage::kAsDeclared;
    bool seen = false;

    size_t start = 0;
    while (start <= document.size()) {
        const size_t nl = document.find('\n', start);
        const size_t end = (nl == std::string::npos) ? document.size() : nl;
        std::string entry = document.substr(start, end - start);
        if (!entry.empty() && entry.back() == '\r') {
            entry.pop_back();
        }
        start = (nl == std::string::npos) ? document.size() + 1 : nl + 1;

        if (entry.empty()) continue;

        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "InProcessPubSubProvider: document entry with no '=': " +
                                  QuoteEntry(entry));
        }
        const std::string key = entry.substr(0, eq);
        const std::string value = entry.substr(eq + 1);

        if (key != "schema_carriage") {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "InProcessPubSubProvider: unknown document key: " +
                                  QuoteEntry(entry));
        }
        if (seen) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "InProcessPubSubProvider: document key 'schema_carriage' given "
                              "twice, at: " +
                                  QuoteEntry(entry));
        }
        seen = true;
        if (value == "as_declared") {
            carriage = SchemaCarriage::kAsDeclared;
        } else if (value == "carried") {
            carriage = SchemaCarriage::kCarried;
        } else {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "InProcessPubSubProvider: unknown value for 'schema_carriage': " +
                                  QuoteEntry(entry));
        }
    }
    return carriage;
}

}  // namespace

void RegisterInProcessProvider(ProviderRegistry& registry) {
    registry.Register("inprocess", [](const ProviderConfig& config) {
        return std::make_shared<InProcessPubSubProvider>(config);
    });
}

struct InProcessPubSubProvider::Impl {
    struct TopicState {
        SubscribeCallback callback;
        // What the LIVE subscription was told, latched when Subscribe returned.
        // Not the same thing as `schema` below: in kAsDeclared a declaration
        // that lands after a subscription exists must never reach it, so the
        // subscription keeps delivering exactly what its SchemaArrival reported
        // until the client resubscribes.
        SharedSchema subscription_schema;
        // The write end of that subscription's arrival, held only in kCarried
        // while no schema has been declared yet. Destroying it unresolved is
        // kSubscriptionEnded, which is what a teardown before a declaration is.
        std::optional<SchemaResolver> resolver;
        // Null when nobody announced one; in kAsDeclared the gateway lets the
        // client bring its own.
        SharedSchema schema;
        // Set once a publisher has declared this topic. Absent for a topic a
        // subscriber or a publish created lazily, which is why it is optional
        // rather than a flag beside empty bytes: "declared with no schema" and
        // "never declared" are different states, and only the first can conflict.
        std::optional<internal::DeclaredSchema> declared;
    };

    explicit Impl(SchemaCarriage carriage_in) : carriage(carriage_in) {}

    SchemaCarriage carriage;
    std::mutex mu;
    std::unordered_map<std::string, TopicState> topics;
};

InProcessPubSubProvider::InProcessPubSubProvider(const ProviderConfig& config)
    : impl_(std::make_unique<Impl>(ParseSchemaCarriage(config.document))) {}

InProcessPubSubProvider::~InProcessPubSubProvider() = default;

void InProcessPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                          OwnedSchema schema) {
    // Every seam entry point translates, so the only exception leaving this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        std::string key = internal::JoinSegments(topic_segments);

        if (impl_->carriage == SchemaCarriage::kCarried && !schema) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "InProcessPubSubProvider: a schema-carrying instance cannot declare "
                              "a topic with no schema: " +
                                  key);
        }

        // Encode before taking the lock, so the locked section is a byte compare
        // rather than an IPC encode every concurrent CreateTopic queues behind.
        // The SAME comparison Publisher::CreateTopic uses one layer up — deliberately
        // not a second implementation of it.
        internal::DeclaredSchema incoming = internal::DeclaredSchema::Encode(schema.get());

        SharedSchema shared;
        if (schema) {
            shared = MakeSharedSchema(OwnedSchema::DeepCopy(schema.get()));
        }

        std::optional<SchemaResolver> to_resolve;
        {
            std::lock_guard lock(impl_->mu);
            auto& slot = impl_->topics[key];

            // Re-declaration is idempotent for an identical schema (so several
            // publishers may share one topic) and REFUSED for a conflicting one —
            // spec §7 clause 3, tightened from "may be rejected" to "must be rejected".
            // This used to overwrite the cached schema silently, which meant a
            // subscriber's next row was decoded against a shape nobody agreed to.
            if (slot.declared.has_value()) {
                if (incoming.ConflictsWith(*slot.declared)) {
                    throw PubSubError(PubSubStatus::kSchemaConflict,
                                      "InProcessPubSubProvider: topic already declared with a "
                                      "conflicting schema: " +
                                          key);
                }
                return;  // identical (or non-comparable) re-declaration — no-op
            }

            slot.declared = std::move(incoming);
            slot.schema = shared;

            // In kCarried a subscription that already exists is still waiting for
            // its schema, so this declaration IS its arrival — and every delivery
            // it ever sees carries this schema, so nothing is mixed.
            //
            // In kAsDeclared it is deliberately NOT: `subscription_schema` is left
            // exactly as Subscribe latched it, so a live subscription cannot flip
            // from null to non-null mid-stream (§7 clause 1). A client that wants
            // the newly declared shape resubscribes.
            if (impl_->carriage == SchemaCarriage::kCarried) {
                slot.subscription_schema = shared;
                if (slot.resolver.has_value()) {
                    to_resolve = std::move(slot.resolver);
                    slot.resolver.reset();
                }
            }
        }

        // Outside the lock: resolving wakes waiters, and none of them should be
        // woken into a thread that still holds the provider mutex.
        if (to_resolve.has_value()) {
            std::move(*to_resolve).Resolve(shared);
        }
    });
}

// mu_ is held across the callback: one delivery at a time, so a callback must not re-enter.
void InProcessPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                      const RowEncoder& encoder, const Attachments& attachments) {
    TranslateSeamFailure([&] {
        VectorWriteBuffer wb;
        encoder(wb);
        const std::vector<uint8_t> buf = wb.Finish();

        std::string key = internal::JoinSegments(topic_segments);

        std::lock_guard lock(impl_->mu);
        auto [it, _] = impl_->topics.try_emplace(std::move(key));

        // Schema-before-data on a carrying instance, held by refusal: there is no
        // implicit declaration, and no sample can be delivered with a schema that
        // does not exist yet.
        if (impl_->carriage == SchemaCarriage::kCarried && !it->second.declared.has_value()) {
            throw PubSubError(
                PubSubStatus::kTopicNotDeclared,
                "InProcessPubSubProvider: publish to an undeclared topic: " + it->first);
        }

        // Copy-to-locals before dispatch (HARD-4's pattern): a callback that re-enters
        // Unsubscribe() would otherwise null the std::function being invoked. Dispatch stays
        // under mu_ so the delivery contract's one-callback-at-a-time clause holds; mu_ is
        // non-recursive, so a re-entering callback deadlocks rather than corrupting — which is
        // what the contract forbids.
        const SubscribeCallback cb = it->second.callback;
        // What this SUBSCRIPTION was told, not what the topic currently holds.
        const SharedSchema schema = it->second.subscription_schema;
        if (cb) {
            cb(buf.data(), buf.size(), schema, attachments);
        }
    });
}

SubscriptionResult InProcessPubSubProvider::Subscribe(
    const std::vector<std::string>& topic_segments, SubscribeCallback callback) {
    return TranslateSeamFailure([&]() -> SubscriptionResult {
        std::string key = internal::JoinSegments(topic_segments);

        std::lock_guard lock(impl_->mu);
        auto& slot = impl_->topics[key];
        slot.callback = std::move(callback);
        // Dropping any previous subscription's resolver reports kSubscriptionEnded
        // to whoever still holds that arrival — one callback per topic per
        // instance (§7 clause 4), so the old subscription really is over.
        slot.resolver.reset();

        if (impl_->carriage == SchemaCarriage::kCarried) {
            if (slot.schema) {
                slot.subscription_schema = slot.schema;
                return SubscriptionResult{SchemaArrival::Ready(slot.schema)};
            }
            // Nothing declared yet: the arrival stays open until a publisher
            // declares the topic, or until this subscription ends.
            auto [arrival, resolver] = SchemaArrival::Create();
            slot.resolver.emplace(std::move(resolver));
            return SubscriptionResult{std::move(arrival)};
        }

        // kAsDeclared: latched HERE, once, for this subscription's whole life —
        // whatever is declared right now, which may legitimately be null.
        slot.subscription_schema = slot.schema;
        return SubscriptionResult{SchemaArrival::Ready(slot.subscription_schema)};
    });
}

void InProcessPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    TranslateSeamFailure([&] {
        std::optional<SchemaResolver> ended;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->topics.find(internal::JoinSegments(topic_segments));
            if (it == impl_->topics.end()) return;
            it->second.callback = nullptr;
            it->second.subscription_schema = nullptr;
            ended = std::move(it->second.resolver);
            it->second.resolver.reset();
        }
        // Destroyed outside the lock: unresolved means kSubscriptionEnded, which
        // is precisely what "torn down before the schema arrived" is.
    });
}

}  // namespace fletcher
