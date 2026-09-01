// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The instrument behind `copy_accounting.hpp`: the probe provider and its two
// control variants, the accounting encoder, and the one pure Judge(). The
// argument for all of it lives in README.md. Deliberately gtest-free — the
// assertions live in `copy_clauses.cpp`.

#include "fletcher/conformance/copy_accounting.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace fletcher {
namespace conformance {
namespace {

/// Sample an address while its storage is still live (see `Address`).
Address At(const void* p) { return reinterpret_cast<Address>(p); }

std::string JoinTopic(const Topic& topic) {
    std::string out;
    for (const std::string& segment : topic) {
        if (!out.empty()) out += '/';
        out += segment;
    }
    return out;
}

/// A fixed arena of slots. The slots are MEMBERS, so P5's liveness precondition
/// holds by construction rather than by argument. Slots rotate so the loaned
/// bytes and the row of one publish never share an address.
class Arena {
   public:
    static constexpr size_t kSlots = 4;
    /// Above kLargeRowBytes; FixedWriteBuffer throws on overflow, so an
    /// undersized slot is a loud failure rather than a silent truncation.
    static constexpr size_t kSlotBytes = 8192;

    uint8_t* NextSlot() {
        uint8_t* base = slots_[cursor_].data();
        cursor_ = (cursor_ + 1) % kSlots;
        return base;
    }

   private:
    std::array<std::array<uint8_t, kSlotBytes>, kSlots> slots_{};
    size_t cursor_ = 0;
};

/// A growable window owned by THIS HARNESS. Every refill allocates the
/// replacement while the old block is still held, so relocation is
/// unconditional and observable (README, refill).
class GrowableProbeBuffer : public WriteBuffer {
   public:
    GrowableProbeBuffer() : WriteBuffer(nullptr, 0) {}

   private:
    static constexpr size_t kStep = 128;

    void AppendSlow(const uint8_t* data, size_t len) override {
        Grow(len);
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendZerosSlow(size_t len) override {
        Grow(len);
        std::memset(data_ + pos_, 0, len);
        pos_ += len;
    }

    void Grow(size_t len) {
        std::vector<uint8_t> next(pos_ + std::max<size_t>(len, kStep));
        if (pos_ > 0) std::memcpy(next.data(), buf_.data(), pos_);
        buf_ = std::move(next);
        data_ = buf_.data();
        capacity_ = buf_.size();
    }

    std::vector<uint8_t> buf_;
};

/// A schema-less transport passes null throughout (spec §7 clause 1); this
/// oracle measures bytes, not schemas.
const SharedSchema& NoSchema() {
    static const SharedSchema kNone{};
    return kNone;
}

/// One class, three behaviours, so no control can drift from the thing it
/// controls: `kZeroCopy` delivers the arena slot itself (positive control — a
/// red there means the measurement is wrong), `kStaging` stages the row and
/// deep-copies every blob (negative control), `kGrowable` encodes into a
/// harness-owned growable window (refill control).
enum class ProbeMode { kZeroCopy, kStaging, kGrowable };

class SeamProbeProvider : public PubSubProvider {
   public:
    explicit SeamProbeProvider(ProbeMode mode) : mode_(mode) {}

    void CreateTopic(const std::vector<std::string>&, OwnedSchema) override {}

    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments) override {
        SubscribeCallback* cb = Callback(topic_segments);

        if (mode_ == ProbeMode::kGrowable) {
            // The buffer outlives the callback, so P5 holds here too.
            GrowableProbeBuffer buffer;
            encoder(buffer);
            if (cb != nullptr) (*cb)(buffer.Data(), buffer.Position(), NoSchema(), attachments);
            return;
        }

        uint8_t* slot = arena_->NextSlot();
        FixedWriteBuffer buffer(slot, Arena::kSlotBytes);
        encoder(buffer);
        if (cb == nullptr) return;

        if (mode_ == ProbeMode::kZeroCopy && loan_len_ == 0) {
            (*cb)(slot, buffer.Position(), NoSchema(), attachments);
            return;
        }

        // The caller's map, copied SHALLOWLY: copying shared_ptrs moves no
        // payload byte, so it is not a copy under this oracle's definition (P3).
        Attachments delivered = attachments;
        if (loan_len_ > 0) {
            // Where §3.2 USED to bite, and now does not: `Blob` is an owner plus
            // a span, so bytes this provider already holds cross where they lie.
            // The owner is the arena itself — a real one, so the blob keeps
            // those bytes alive past the delivery exactly as a transport loan
            // handle would.
            delivered.insert_or_assign(loan_key_, Blob(arena_, loan_base_, loan_len_));
        }

        if (mode_ != ProbeMode::kStaging) {
            (*cb)(slot, buffer.Position(), NoSchema(), delivered);
            return;
        }

        // The control's whole job: move every payload byte to a second address
        // while keeping the content identical, so `memcmp` cannot tell the
        // difference and only provenance can.
        const std::vector<uint8_t> staged(slot, slot + buffer.Position());
        Attachments deep;
        for (const auto& [key, blob] : delivered) {
            deep.emplace(key, blob.empty() ? blob
                                           : Blob(std::vector<uint8_t>(blob.data(),
                                                                       blob.data() + blob.size())));
        }
        (*cb)(staged.data(), staged.size(), NoSchema(), deep);
    }

    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override {
        callbacks_[JoinTopic(topic_segments)] = std::move(callback);
        // Schema-less by construction (§7 clause 1): kOk with a null schema.
        return {SchemaArrival::Ready(nullptr)};
    }

    void Unsubscribe(const std::vector<std::string>& topic_segments) override {
        callbacks_.erase(JoinTopic(topic_segments));
    }

    /// Park `payload` in an arena slot — memory this provider owns and a real
    /// transport would have been LOANED — and make every later `Publish`
    /// deliver those bytes under `key`. Returns the arena base: the address the
    /// delivered blob would carry if the seam could carry borrowed memory.
    const uint8_t* LoanForDelivery(std::string key, const std::vector<uint8_t>& payload) {
        if (payload.size() > Arena::kSlotBytes) {
            throw std::overflow_error("SeamProbeProvider::LoanForDelivery: slot overflow");
        }
        uint8_t* base = arena_->NextSlot();
        std::memcpy(base, payload.data(), payload.size());
        loan_key_ = std::move(key);
        loan_base_ = base;
        loan_len_ = payload.size();
        return base;
    }

   private:
    SubscribeCallback* Callback(const std::vector<std::string>& topic_segments) {
        auto it = callbacks_.find(JoinTopic(topic_segments));
        if (it == callbacks_.end() || !it->second) return nullptr;
        return &it->second;
    }

    // Held by shared_ptr because a Blob handed over from it must be able to OWN
    // it: that is the whole §3.2 contract a transport loan has to satisfy, and
    // the probe has to satisfy it too or it is not standing in for one.
    std::shared_ptr<Arena> arena_ = std::make_shared<Arena>();
    ProbeMode mode_;
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::string loan_key_;
    const uint8_t* loan_base_ = nullptr;
    size_t loan_len_ = 0;
};

/// Calls the provider directly at the seam.
class DirectRunner : public CopyRunner {
   public:
    explicit DirectRunner(std::shared_ptr<PubSubProvider> provider)
        : provider_(std::move(provider)) {}

    void Subscribe(const Topic& topic, PubSubProvider::SubscribeCallback cb) override {
        (void)provider_->Subscribe(topic, std::move(cb));
    }

    void Publish(const Topic& topic, const PubSubProvider::RowEncoder& encoder,
                 const Attachments& attachments) override {
        provider_->Publish(topic, encoder, attachments);
    }

    void Unsubscribe(const Topic& topic) override { provider_->Unsubscribe(topic); }

   private:
    std::shared_ptr<PubSubProvider> provider_;
};

/// Routes through `Publisher` and `Subscriber`, so the layers ABOVE the seam sit
/// inside a measured path: without it a `std::vector` materialised in
/// `Subscriber`'s fan-out "for safety" would be invisible.
class PubSubStackRunner : public CopyRunner {
   public:
    explicit PubSubStackRunner(std::shared_ptr<PubSubProvider> provider)
        : publisher_(provider), subscriber_(provider) {}

    void Subscribe(const Topic& topic, PubSubProvider::SubscribeCallback cb) override {
        Subscriber::SubscribeResult result = subscriber_.Subscribe(
            topic, [cb = std::move(cb)](uint64_t, const uint8_t* data, size_t len,
                                        const SharedSchema& schema,
                                        const Attachments& att) { cb(data, len, schema, att); });
        subscription_id_ = result.subscription_id;
    }

    void Publish(const Topic& topic, const PubSubProvider::RowEncoder& encoder,
                 const Attachments& attachments) override {
        publisher_.Publish(topic, encoder, attachments);
    }

    void Unsubscribe(const Topic&) override {
        if (subscription_id_ != 0) subscriber_.Unsubscribe(subscription_id_);
        subscription_id_ = 0;
    }

   private:
    Publisher publisher_;
    Subscriber subscriber_;
    uint64_t subscription_id_ = 0;
};

/// Writes `payload` through the seam's own `Append` in `kAppendChunk`-sized
/// pieces, sampling the window base either side of each one. A base that changes
/// while `Position() > 0` relocated the bytes already written, and the prior
/// position is how many moved; a change at `Position() == 0` moved nothing.
void EncodeAccounted(WriteBuffer& buffer, const std::vector<uint8_t>& payload, CopyLedger& ledger) {
    for (size_t offset = 0; offset < payload.size(); offset += kAppendChunk) {
        const size_t take = std::min(kAppendChunk, payload.size() - offset);
        const Address base_before = At(buffer.Data());
        const size_t pos_before = buffer.Position();

        buffer.Append(payload.data() + offset, take);

        if (At(buffer.Data()) != base_before && pos_before > 0) {
            ++ledger.refill_moves;
            ledger.refill_bytes += pos_before;
        }
    }
    // AFTER the last append: this is the window the delivered bytes must be.
    ledger.encode_base = At(buffer.Data());
    ledger.encode_len = buffer.Position();
}

/// Delivery-side capture, shared by every leg — one capture path, so no per-path
/// branch exists for a missed copy to hide in. `ledger.attachments` must already
/// carry the PUBLISHED side; this fills in the delivered side and compares
/// content here, where the pointers are still borrowed for the call.
PubSubProvider::SubscribeCallback MakeCapture(CopyLedger& ledger,
                                              const std::vector<uint8_t>& expected_row,
                                              Blob& retained) {
    return [&ledger, &expected_row, &retained](const uint8_t* data, size_t len, const SharedSchema&,
                                               const Attachments& attachments) {
        ++ledger.deliveries;

        // P5, checked first and while the window is live by precondition: a
        // subject that freed or recycled it will almost never hand back
        // byte-identical contents, so the likely violation fails as itself.
        const auto* window = reinterpret_cast<const uint8_t*>(ledger.encode_base);
        ledger.window_intact = window != nullptr && ledger.encode_len == expected_row.size() &&
                               std::memcmp(window, expected_row.data(), ledger.encode_len) == 0;

        ledger.delivered_data = At(data);
        ledger.delivered_len = len;
        ledger.row_content_ok = len == expected_row.size() && data != nullptr &&
                                (len == 0 || std::memcmp(data, expected_row.data(), len) == 0);
        ledger.delivered_attachments = attachments.size();

        for (AttachmentTrace& trace : ledger.attachments) {
            // Left at 0 when the key is absent: MISSING, a different failure
            // from garbled, and Judge() scores it as a copy either way.
            auto it = attachments.find(trace.key);
            if (it == attachments.end() || it->second.data() == nullptr) continue;
            trace.delivered_data = At(it->second.data());
            trace.delivered_len = it->second.size();
            const auto* published = reinterpret_cast<const uint8_t*>(trace.published_data);
            trace.content_ok =
                trace.delivered_len == trace.published_len &&
                (trace.published_len == 0 ||
                 std::memcmp(it->second.data(), published, trace.published_len) == 0);
        }

        // §3.2 clause 1: a callee that wants to keep a borrowed blob takes its
        // own reference. Done HERE, inside the borrow window, because that is
        // the only place the rule permits it.
        if (!ledger.retain_key.empty()) {
            auto it = attachments.find(ledger.retain_key);
            if (it != attachments.end()) retained = it->second;
        }
    };
}

RoundTrip RunCaptured(CopyRunner& runner, const Topic& topic, const std::vector<uint8_t>& payload,
                      const Attachments& attachments, CopyLedger ledger) {
    RoundTrip trip;
    trip.ledger = std::move(ledger);

    // Outlives the delivery below, so what it reports is read strictly after the
    // callback returned and the transport's borrow window closed.
    Blob retained;

    runner.Subscribe(topic, MakeCapture(trip.ledger, payload, retained));
    try {
        runner.Publish(
            topic,
            [&payload, &trip](WriteBuffer& buffer) {
                EncodeAccounted(buffer, payload, trip.ledger);
            },
            attachments);
    } catch (const std::exception& e) {
        trip.error = DescribeException(e);
    } catch (...) {
        trip.error = "unknown exception";
    }
    runner.Unsubscribe(topic);

    // AFTER the callback, after Unsubscribe: if the blob's owner is real these
    // bytes are still readable, still at the address they were published at.
    if (!trip.ledger.retain_key.empty() && retained.data() != nullptr) {
        trip.ledger.retained_data = At(retained.data());
        const AttachmentTrace* trace = nullptr;
        for (const AttachmentTrace& t : trip.ledger.attachments) {
            if (t.key == trip.ledger.retain_key) trace = &t;
        }
        if (trace != nullptr) {
            const auto* published = reinterpret_cast<const uint8_t*>(trace->published_data);
            trip.ledger.retained_content_ok =
                retained.size() == trace->published_len &&
                (trace->published_len == 0 ||
                 std::memcmp(retained.data(), published, trace->published_len) == 0);
        }
    }
    return trip;
}

}  // namespace

CopyVerdict Judge(const CopyLedger& ledger) {
    CopyVerdict verdict;

    // Strict equality, not containment — see the header for the in-place
    // memmove that containment would score as zero.
    const bool row_is_the_encode_window = ledger.delivered_data != 0 &&
                                          ledger.delivered_data == ledger.encode_base &&
                                          ledger.delivered_len == ledger.encode_len;
    verdict.row_copies = row_is_the_encode_window ? 0 : 1;

    for (const AttachmentTrace& trace : ledger.attachments) {
        const bool same_bytes = trace.delivered_data != 0 &&
                                trace.delivered_data == trace.published_data &&
                                trace.delivered_len == trace.published_len;
        if (!same_bytes) ++verdict.attachment_copies;
    }

    verdict.refill_moves = ledger.refill_moves;
    verdict.refill_bytes = ledger.refill_bytes;
    return verdict;
}

std::vector<uint8_t> CopyPayload(size_t len) {
    std::vector<uint8_t> payload(len);
    for (size_t i = 0; i < len; ++i) {
        payload[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    }
    return payload;
}

Attachments MakeCopyAttachments() {
    Attachments attachments;
    for (size_t i = 0; i < kAttachmentCount; ++i) {
        std::vector<uint8_t> bytes = CopyPayload(kAttachmentBytes);
        bytes[0] = static_cast<uint8_t>(i);
        attachments.emplace("blob" + std::to_string(i), Blob(std::move(bytes)));
    }
    return attachments;
}

RoundTrip RunRoundTrip(CopyRunner& runner, const Topic& topic, size_t row_bytes,
                       const Attachments& attachments) {
    const std::vector<uint8_t> payload = CopyPayload(row_bytes);

    CopyLedger ledger;
    // The published side comes from the CALLER's blobs, so leg 2 compares the
    // delivered data() against the published data() and never against a
    // re-derivation of it.
    for (const auto& [key, blob] : attachments) {
        AttachmentTrace trace;
        trace.key = key;
        trace.published_data = At(blob.data());
        trace.published_len = blob.size();
        ledger.attachments.push_back(std::move(trace));
    }
    return RunCaptured(runner, topic, payload, attachments, std::move(ledger));
}

RoundTrip RunBorrowedAttachmentRoundTrip(const Topic& topic, bool copying_provider) {
    auto provider = std::make_shared<SeamProbeProvider>(copying_provider ? ProbeMode::kStaging
                                                                         : ProbeMode::kZeroCopy);

    // Bytes the provider already holds, where a transport's loaned sample would
    // be. The provider turns them into a Blob inside Publish; the harness never
    // constructs that Blob.
    const std::vector<uint8_t> loaned = CopyPayload(kAttachmentBytes);
    const uint8_t* loaned_base = provider->LoanForDelivery("loaned", loaned);

    // A caller-owned blob rides along on the same publish. It must cross
    // untouched, so a provider that copies indiscriminately scores 2 and not 1.
    std::vector<uint8_t> owned_bytes = CopyPayload(kAttachmentBytes);
    owned_bytes[0] = 0xEE;
    Attachments attachments;
    attachments.emplace("owned", Blob(std::move(owned_bytes)));

    CopyLedger ledger;
    AttachmentTrace owned;
    owned.key = "owned";
    owned.published_data = At(attachments.at("owned").data());
    owned.published_len = attachments.at("owned").size();
    ledger.attachments.push_back(std::move(owned));

    // The borrowed entry is the one kept past the callback: it is the entry whose
    // bytes the provider does not own a `vector` of.
    ledger.retain_key = "loaned";

    AttachmentTrace borrowed;
    borrowed.key = "loaned";
    // The published address is the LOANED base, not any Blob's — that is the
    // whole question: could the seam have carried those bytes as they lay?
    borrowed.published_data = At(loaned_base);
    borrowed.published_len = loaned.size();
    ledger.attachments.push_back(std::move(borrowed));

    DirectRunner runner(provider);
    const std::vector<uint8_t> payload = CopyPayload(kSmallRowBytes);
    return RunCaptured(runner, topic, payload, attachments, std::move(ledger));
}

const std::vector<CopySubject>& CopyAccountingSubjects() {
    static const std::vector<CopySubject> kSubjects = {
        CopySubject{"SeamProbe",
                    [] {
                        return std::unique_ptr<CopyRunner>(std::make_unique<DirectRunner>(
                            std::make_shared<SeamProbeProvider>(ProbeMode::kZeroCopy)));
                    }},
        CopySubject{"InProcessLoopback",
                    [] {
                        return std::unique_ptr<CopyRunner>(std::make_unique<DirectRunner>(
                            std::make_shared<InProcessPubSubProvider>()));
                    }},
        CopySubject{"InProcessViaPubSub",
                    [] {
                        return std::unique_ptr<CopyRunner>(std::make_unique<PubSubStackRunner>(
                            std::make_shared<InProcessPubSubProvider>()));
                    }},
    };
    return kSubjects;
}

CopySubject StagingControlSubject() {
    return CopySubject{"StagingProbe", [] {
                           return std::unique_ptr<CopyRunner>(std::make_unique<DirectRunner>(
                               std::make_shared<SeamProbeProvider>(ProbeMode::kStaging)));
                       }};
}

CopySubject GrowableControlSubject() {
    return CopySubject{"GrowableProbe", [] {
                           return std::unique_ptr<CopyRunner>(std::make_unique<DirectRunner>(
                               std::make_shared<SeamProbeProvider>(ProbeMode::kGrowable)));
                       }};
}

}  // namespace conformance
}  // namespace fletcher
