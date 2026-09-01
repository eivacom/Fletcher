// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The instrument behind `copy_accounting.hpp`: the arena-backed SeamProbe, the
// deliberately-copying StagingProbe that is the live negative control, the
// accounting encoder that samples window relocations, and the one pure Judge()
// every subject and the control are scored by.
//
// Deliberately gtest-free — the assertions live in `copy_clauses.cpp`, so the
// instrument can be reasoned about (and, in Judge()'s case, unit-tested) without
// a test framework in the way.

#include "fletcher/conformance/copy_accounting.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace fletcher {
namespace conformance {
namespace {

std::string JoinTopic(const Topic& topic) {
    std::string out;
    for (const std::string& segment : topic) {
        if (!out.empty()) out += '/';
        out += segment;
    }
    return out;
}

/// A fixed arena of slots. The slots are MEMBERS, so their bytes stay allocated
/// for the provider's whole lifetime and P5's liveness precondition holds by
/// construction rather than by argument.
///
/// Slots rotate rather than being reused immediately: a copy that happened to
/// land back on the previous publish's address would then be visible as a
/// difference, instead of being masked.
class Arena {
   public:
    static constexpr size_t kSlots = 4;
    /// Comfortably above kLargeRowBytes; a FixedWriteBuffer throws on overflow,
    /// so an undersized slot is a loud failure, not a silent truncation.
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

/// A schema-less transport passes null throughout (spec §7 clause 1); this
/// oracle measures bytes, not schemas.
const SharedSchema& NoSchema() {
    static const SharedSchema kNone{};
    return kNone;
}

/// The harness's own minimal provider. Not a conformance subject — it
/// implements exactly what the copy oracle exercises (one callback per topic,
/// synchronous delivery on the publishing thread) and nothing else.
///
/// `Publish` takes a slot from the arena, hands the encoder a `FixedWriteBuffer`
/// over it and delivers `slot` itself, so it proves the seam PERMITS zero-copy:
/// a red on this subject means the measurement, not the provider, is wrong.
///
/// With `stage_copies` it becomes the negative control instead: same arena, same
/// delivery, but the row is staged through a scratch vector and every blob is
/// deep-copied first. One class, two behaviours, so the control cannot drift
/// away from the thing it controls.
class SeamProbeProvider : public PubSubProvider {
   public:
    explicit SeamProbeProvider(bool stage_copies) : stage_copies_(stage_copies) {}

    void CreateTopic(const std::vector<std::string>&, OwnedSchema) override {}

    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments) override {
        uint8_t* slot = arena_.NextSlot();
        FixedWriteBuffer buffer(slot, Arena::kSlotBytes);
        encoder(buffer);

        auto it = callbacks_.find(JoinTopic(topic_segments));
        if (it == callbacks_.end() || !it->second) return;

        if (!stage_copies_) {
            it->second(slot, buffer.Position(), NoSchema(), attachments);
            return;
        }

        // The control's whole job: move every payload byte to a second address
        // while keeping the content identical, so `memcmp` cannot tell the
        // difference and only provenance can.
        std::vector<uint8_t> staged(slot, slot + buffer.Position());
        Attachments deep;
        for (const auto& [key, blob] : attachments) {
            deep.emplace(key, blob ? std::make_shared<const std::vector<uint8_t>>(*blob) : blob);
        }
        it->second(staged.data(), staged.size(), NoSchema(), deep);
    }

    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override {
        callbacks_[JoinTopic(topic_segments)] = std::move(callback);
        return {MakeReadySchemaFuture(nullptr)};
    }

    void Unsubscribe(const std::vector<std::string>& topic_segments) override {
        callbacks_.erase(JoinTopic(topic_segments));
    }

    /// Fill an arena slot with `len` payload bytes and return its base — memory
    /// this provider owns and would, in a real transport, have been LOANED by
    /// the reader. Leg 3's stand-in for a loaned sample.
    const uint8_t* StageLoanedBytes(const std::vector<uint8_t>& payload) {
        uint8_t* base = arena_.NextSlot();
        std::memcpy(base, payload.data(), payload.size());
        return base;
    }

   private:
    Arena arena_;
    bool stage_copies_;
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
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
/// inside a measured path. §8 words the attachment claim "publisher → provider →
/// subscriber"; without this runner nothing above the seam is measured, and a
/// `std::vector` materialised in `Subscriber`'s fan-out "for safety" would be
/// invisible to the oracle that exists to catch exactly that.
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

/// Writes `payload` through the seam's own `Append`, in `kAppendChunk`-sized
/// pieces, sampling the window base on either side of every append.
///
/// A base that changes while `Position() > 0` is a refill that RELOCATED the
/// bytes already written — permitted by §3.1 clause 1 and by the owner's
/// 2026-09-01 ruling, on condition that its cost is published as a number. The
/// prior position is exactly how many bytes moved. A base that changes at
/// `Position() == 0` moved nothing and is correctly not counted.
void EncodeAccounted(WriteBuffer& buffer, const std::vector<uint8_t>& payload, CopyLedger& ledger) {
    for (size_t offset = 0; offset < payload.size(); offset += kAppendChunk) {
        const size_t take = std::min(kAppendChunk, payload.size() - offset);
        const uint8_t* base_before = buffer.Data();
        const size_t pos_before = buffer.Position();

        buffer.Append(payload.data() + offset, take);

        if (buffer.Data() != base_before && pos_before > 0) {
            ++ledger.refill_moves;
            ledger.refill_bytes += pos_before;
        }
    }
    // AFTER the last append: this is the window the delivered bytes must be.
    ledger.encode_base = buffer.Data();
    ledger.encode_len = buffer.Position();
}

/// Delivery-side capture, shared by every leg — one capture path, so there is no
/// per-path branch for a missed copy to hide in.
///
/// `ledger.attachments` must already carry the PUBLISHED side of each trace;
/// this fills in the delivered side and does the content compare. The compare
/// happens here because the pointers are borrowed for the call only.
PubSubProvider::SubscribeCallback MakeCapture(CopyLedger& ledger,
                                              const std::vector<uint8_t>& expected_row) {
    return [&ledger, &expected_row](const uint8_t* data, size_t len, const SharedSchema&,
                                    const Attachments& attachments) {
        ++ledger.deliveries;
        ledger.delivered_data = data;
        ledger.delivered_len = len;
        ledger.row_content_ok = len == expected_row.size() && data != nullptr &&
                                (len == 0 || std::memcmp(data, expected_row.data(), len) == 0);

        for (AttachmentTrace& trace : ledger.attachments) {
            auto it = attachments.find(trace.key);
            if (it == attachments.end() || !it->second) continue;
            trace.delivered_data = it->second->data();
            trace.delivered_len = it->second->size();
            trace.content_ok =
                trace.delivered_len == trace.published_len &&
                std::memcmp(trace.delivered_data, trace.published_data, trace.published_len) == 0;
        }
    };
}

RoundTrip RunCaptured(CopyRunner& runner, const Topic& topic, const std::vector<uint8_t>& payload,
                      const Attachments& attachments, CopyLedger ledger) {
    RoundTrip trip;
    trip.ledger = std::move(ledger);

    runner.Subscribe(topic, MakeCapture(trip.ledger, payload));
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
    return trip;
}

}  // namespace

CopyVerdict Judge(const CopyLedger& ledger) {
    CopyVerdict verdict;

    // Strict equality, not containment — see the header for the in-place
    // memmove that containment would score as zero.
    const bool row_is_the_encode_window = ledger.delivered_data != nullptr &&
                                          ledger.delivered_data == ledger.encode_base &&
                                          ledger.delivered_len == ledger.encode_len;
    verdict.row_copies = row_is_the_encode_window ? 0 : 1;

    for (const AttachmentTrace& trace : ledger.attachments) {
        const bool same_bytes = trace.delivered_data != nullptr &&
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
        attachments.emplace("blob" + std::to_string(i),
                            std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
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
        trace.published_data = blob ? blob->data() : nullptr;
        trace.published_len = blob ? blob->size() : 0;
        ledger.attachments.push_back(std::move(trace));
    }
    return RunCaptured(runner, topic, payload, attachments, std::move(ledger));
}

RoundTrip RunBorrowedAttachmentRoundTrip(const Topic& topic) {
    auto provider = std::make_shared<SeamProbeProvider>(/*stage_copies=*/false);

    // Bytes the provider already holds, in memory it owns — where a transport's
    // loaned sample would be. Publishing them means putting them in a Blob, and
    // `shared_ptr<const vector<uint8_t>>` cannot alias foreign memory (§3.2), so
    // the vector construction below IS the copy this leg measures. It is the one
    // copy PDA-DEC-3 exists to remove.
    const std::vector<uint8_t> loaned = CopyPayload(kAttachmentBytes);
    const uint8_t* loaned_base = provider->StageLoanedBytes(loaned);

    Attachments attachments;
    attachments.emplace("loaned", std::make_shared<const std::vector<uint8_t>>(
                                      loaned_base, loaned_base + loaned.size()));

    CopyLedger ledger;
    AttachmentTrace trace;
    trace.key = "loaned";
    // The published address is the LOANED base, not the Blob's — that is the
    // whole question: could the seam have carried those bytes as they lay?
    trace.published_data = loaned_base;
    trace.published_len = loaned.size();
    ledger.attachments.push_back(std::move(trace));

    DirectRunner runner(provider);
    const std::vector<uint8_t> payload = CopyPayload(kSmallRowBytes);
    return RunCaptured(runner, topic, payload, attachments, std::move(ledger));
}

const std::vector<CopySubject>& CopyAccountingSubjects() {
    static const std::vector<CopySubject> kSubjects = {
        CopySubject{"SeamProbe",
                    [] {
                        return std::unique_ptr<CopyRunner>(std::make_unique<DirectRunner>(
                            std::make_shared<SeamProbeProvider>(/*stage_copies=*/false)));
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
                               std::make_shared<SeamProbeProvider>(/*stage_copies=*/true)));
                       }};
}

}  // namespace conformance
}  // namespace fletcher
