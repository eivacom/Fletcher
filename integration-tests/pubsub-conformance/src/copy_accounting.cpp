// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The harness half of `copy_accounting.hpp`: the runners, the accounting
// encoder and the round-trip drivers. The probe provider, its control variants
// and the one pure Judge() are `fletcher-copy-probe`'s (D-BIND-62). The argument
// for all of it lives in README.md. Deliberately gtest-free — the assertions
// live in `copy_clauses.cpp`.

#include "fletcher/conformance/copy_accounting.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <fletcher/copy_probe/seam_probe_provider.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace fletcher {
namespace conformance {
namespace {

// The probe and its control variants live in `fletcher-copy-probe` since
// D-BIND-62, with the ledger; the runners and drivers below are this harness's.
using copy_probe::At;
using copy_probe::ProbeMode;
using copy_probe::SeamProbeProvider;

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

/// The CLIENT half of the send path, instrumented — a stand-in for the language
/// binding that does not exist yet, which is exactly what the guard may claim
/// and no more (owner ruling 2026-09-04).
///
/// `kInPlace` composes the row into the span the buffer lends it. `kStaged`
/// composes the identical row in its own vector and hands it over with one
/// `Append`. Both end by sampling the window base, so the PROVIDER half
/// (`row_copies`) is measured identically for the two and only the producer
/// half can tell them apart — which is the whole point of the leg.
void EncodeProduced(WriteBuffer& buffer, const std::vector<uint8_t>& payload, CopyLedger& ledger,
                    ProducerMode mode) {
    const Address base_before = At(buffer.Data());
    const size_t pos_before = buffer.Position();

    if (mode == ProducerMode::kInPlace) {
        buffer.AppendInPlace(payload.size(), [&](uint8_t* dst, size_t room) -> size_t {
            // Sampled HERE, inside the borrow, because nowhere else can answer
            // it: after the call the cursor has already moved past the span.
            // `Data()`/`Position()` are const reads and stay legal inside the
            // writer — that permission is what makes this measurable at all.
            ledger.produced_at = At(dst);
            ledger.produced_in_window = At(dst) == At(buffer.Data()) + buffer.Position();
            // `room >= min_bytes` is the member's own guarantee, so a short span
            // is a broken buffer, not a short row. Fail on it rather than
            // truncating: a silent `take = room` would deliver a partial row and
            // score it as a clean uncopied send.
            if (room < payload.size()) {
                throw std::logic_error(
                    "CopyAccounting: AppendInPlace lent less room than it was asked for");
            }
            std::memcpy(dst, payload.data(), payload.size());
            ledger.produced_len = payload.size();
            return payload.size();
        });
    } else {
        // The workaround a binding was forced into: the row exists at a second
        // address before the seam ever sees it.
        const std::vector<uint8_t> staged = payload;
        ledger.produced_at = At(staged.data());
        ledger.produced_len = staged.size();
        ledger.produced_in_window = At(staged.data()) == At(buffer.Data()) + buffer.Position();
        buffer.Append(staged.data(), staged.size());
    }

    if (At(buffer.Data()) != base_before && pos_before > 0) {
        ++ledger.refill_moves;
        ledger.refill_bytes += pos_before;
    }

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
            const Blob* found = attachments.Find(trace.key);
            if (found == nullptr || found->data() == nullptr) continue;
            trace.delivered_data = At(found->data());
            trace.delivered_len = found->size();
            const auto* published = reinterpret_cast<const uint8_t*>(trace.published_data);
            trace.content_ok = trace.delivered_len == trace.published_len &&
                               (trace.published_len == 0 ||
                                std::memcmp(found->data(), published, trace.published_len) == 0);
        }

        // §3.2 clause 1: a callee that wants to keep a borrowed blob takes its
        // own reference. Done HERE, inside the borrow window, because that is
        // the only place the rule permits it.
        if (!ledger.retain_key.empty()) {
            const Blob* found = attachments.Find(ledger.retain_key);
            if (found != nullptr) retained = *found;
        }
    };
}

/// PHASE 1 — the only code in this file that may touch the subject.
///
/// It takes the runner by reference and NEVER releases anything, so there is no
/// point inside it at which that reference is dead. It used to end with a
/// `release_subject` hook that destroyed the runner: from that hook onwards the
/// parameter was a dangling reference, and any line added below it would have
/// acquired a use-after-free silently. Destruction now belongs to whoever OWNS
/// the subject (see RunBorrowedAttachmentRoundTrip), which is the only place that
/// can do it safely — so the bad state is not guarded against, it cannot be
/// written.
///
/// The ENCODER is a parameter so the producer legs share this one phase-1 path
/// and this one capture rather than growing a second driver: a per-path branch
/// is where a missed copy hides. `EncodeAccounted` is what every pre-existing
/// leg passes, unchanged.
void DriveRoundTrip(CopyRunner& runner, const Topic& topic, const std::vector<uint8_t>& payload,
                    const Attachments& attachments, RoundTrip& trip, Blob& retained,
                    const std::function<void(WriteBuffer&)>& encode) {
    runner.Subscribe(topic, MakeCapture(trip.ledger, payload, retained));
    try {
        runner.Publish(topic, [&encode](WriteBuffer& buffer) { encode(buffer); }, attachments);
    } catch (const std::exception& e) {
        trip.error = DescribeException(e);
    } catch (...) {
        trip.error = "unknown exception";
    }
    runner.Unsubscribe(topic);
}

/// PHASE 2 — reads the blob the callback kept. **Deliberately has no subject
/// parameter**: it is called once the subject is gone, and it must not be able to
/// reach one even by accident.
///
/// For the borrowed leg the caller destroys the provider between the two phases,
/// so what this reads is a blob whose own owner is the last thing keeping those
/// bytes alive. Read with the provider still up, both fields below would pass for
/// a span with no owner at all — the exact case they claim to distinguish.
void ReadRetainedBlob(CopyLedger& ledger, const Blob& retained) {
    if (ledger.retain_key.empty() || retained.data() == nullptr) return;

    ledger.retained_data = At(retained.data());
    // Compared against the HARNESS's own copy of the expected bytes, never
    // against the published address — see CopyLedger::retain_expected. The
    // published address is where the retained blob points, so comparing the
    // two is memcmp(p, p, n) and passes for a dead owner too.
    const std::vector<uint8_t>& expected = ledger.retain_expected;
    ledger.retained_content_ok =
        !expected.empty() && retained.size() == expected.size() &&
        std::memcmp(retained.data(), expected.data(), expected.size()) == 0;
}

}  // namespace

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
        attachments.Set("blob" + std::to_string(i), Blob(std::move(bytes)));
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
    for (size_t i = 0; i < attachments.size(); ++i) {
        const Blob& blob = attachments.ValueAt(i);
        AttachmentTrace trace;
        trace.key = std::string(attachments.KeyAt(i));
        trace.published_data = At(blob.data());
        trace.published_len = blob.size();
        ledger.attachments.push_back(std::move(trace));
    }
    RoundTrip trip;
    trip.ledger = std::move(ledger);
    Blob retained;
    DriveRoundTrip(
        runner, topic, payload, attachments, trip, retained,
        [&payload, &trip](WriteBuffer& buffer) { EncodeAccounted(buffer, payload, trip.ledger); });
    // This path registers no retain_key, so phase 2 is a no-op; it is called
    // anyway so both paths read the ledger through exactly one function.
    ReadRetainedBlob(trip.ledger, retained);
    return trip;
}

RoundTrip RunProducerRoundTrip(CopyRunner& runner, const Topic& topic, size_t row_bytes,
                               ProducerMode mode) {
    const std::vector<uint8_t> payload = CopyPayload(row_bytes);

    RoundTrip trip;
    Blob retained;
    // No attachments: this leg measures the ROW half of §8. Same phase-1 driver,
    // same capture, same Judge() — only the producer differs.
    DriveRoundTrip(runner, topic, payload, Attachments{}, trip, retained,
                   [&payload, &trip, mode](WriteBuffer& buffer) {
                       EncodeProduced(buffer, payload, trip.ledger, mode);
                   });
    ReadRetainedBlob(trip.ledger, retained);
    return trip;
}

RoundTrip RunCustomProducerRoundTrip(CopyRunner& runner, const Topic& topic,
                                     const std::vector<uint8_t>& payload,
                                     const std::function<ProducedRow(uint8_t*, size_t)>& produce) {
    RoundTrip trip;
    Blob retained;
    DriveRoundTrip(runner, topic, payload, Attachments{}, trip, retained,
                   [&payload, &trip, &produce](WriteBuffer& buffer) {
                       const Address base_before = At(buffer.Data());
                       const size_t pos_before = buffer.Position();

                       buffer.AppendInPlace(
                           payload.size(), [&](uint8_t* dst, size_t room) -> size_t {
                               if (room < payload.size()) {
                                   throw std::logic_error(
                                       "CopyAccounting: AppendInPlace lent less room than it was "
                                       "asked for");
                               }
                               // The cursor is read BEFORE the producer runs, because afterwards
                               // it has moved past the span.
                               const Address cursor = At(buffer.Data()) + buffer.Position();
                               const ProducedRow produced = produce(dst, room);
                               // The producer's OWN answer, never the span it was lent.
                               // Recording `dst` here would make `produced_in_window` true by
                               // construction and every foreign producer a zero-copy one — a
                               // tautology wearing a measurement's clothes. `kStaged` reports
                               // its staging address for exactly this reason.
                               trip.ledger.produced_at = At(produced.at);
                               trip.ledger.produced_in_window = At(produced.at) == cursor;
                               trip.ledger.produced_len = produced.len;
                               return produced.len;
                           });

                       if (At(buffer.Data()) != base_before && pos_before > 0) {
                           ++trip.ledger.refill_moves;
                           trip.ledger.refill_bytes += pos_before;
                       }
                       trip.ledger.encode_base = At(buffer.Data());
                       trip.ledger.encode_len = buffer.Position();
                   });
    ReadRetainedBlob(trip.ledger, retained);
    return trip;
}

RoundTrip RunBorrowedAttachmentRoundTrip(const Topic& topic, bool copying_provider) {
    auto provider = std::make_shared<SeamProbeProvider>(copying_provider ? ProbeMode::kStaging
                                                                         : ProbeMode::kZeroCopy);
    // Watched, not merely dropped: the release below reports whether the probe
    // actually died, so a stray keep-alive shows up as a failed assertion rather
    // than as a leg that quietly stops testing anything. A weak_ptr and nothing
    // else — the runner takes the only strong reference, so destroying it is
    // sufficient and this local cannot accidentally keep the arena alive.
    std::weak_ptr<SeamProbeProvider> watch = provider;

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
    attachments.Set("owned", Blob(std::move(owned_bytes)));

    CopyLedger ledger;
    AttachmentTrace owned;
    owned.key = "owned";
    const Blob* owned_blob = attachments.Find("owned");
    // Absence is a null pointer, not a throw (§3.2), so an absent entry would
    // segfault this harness instead of failing it. This function returns a value,
    // so a gtest ASSERT cannot be used here; the throw reaches the same red.
    if (owned_blob == nullptr)
        throw std::logic_error("the 'owned' attachment just Set is not in the set");
    owned.published_data = At(owned_blob->data());
    owned.published_len = owned_blob->size();
    ledger.attachments.push_back(std::move(owned));

    // The borrowed entry is the one kept past the callback: it is the entry whose
    // bytes the provider does not own a `vector` of.
    ledger.retain_key = "loaned";
    ledger.retain_expected = loaned;

    AttachmentTrace borrowed;
    borrowed.key = "loaned";
    // The published address is the LOANED base, not any Blob's — that is the
    // whole question: could the seam have carried those bytes as they lay?
    borrowed.published_data = At(loaned_base);
    borrowed.published_len = loaned.size();
    ledger.attachments.push_back(std::move(borrowed));

    auto runner = std::make_unique<DirectRunner>(std::move(provider));
    const std::vector<uint8_t> payload = CopyPayload(kSmallRowBytes);

    RoundTrip trip;
    trip.ledger = std::move(ledger);
    // Outlives both phases, so what it reports is read strictly after the callback
    // returned, after Unsubscribe, and after the subject below is destroyed.
    Blob retained;

    // Phase 1, in its own scope: `live` is the only name bound to the subject, and
    // it does not exist past this brace. Nothing added after it can reach the
    // runner even by mistake.
    {
        CopyRunner& live = *runner;
        DriveRoundTrip(live, topic, payload, attachments, trip, retained,
                       [&payload, &trip](WriteBuffer& buffer) {
                           EncodeAccounted(buffer, payload, trip.ledger);
                       });
    }

    // The subject dies HERE, in the function that owns it — the only place that
    // can destroy it without leaving a dangling reference behind. `provider` was
    // moved into the runner above, so this is the last strong reference; from now
    // on the only thing that can be keeping the loaned bytes alive is the Blob's
    // own owner, which is the whole claim.
    runner.reset();
    trip.ledger.subject_released = watch.expired();

    // Phase 2 — reads the retained blob, with no way to reach a subject.
    ReadRetainedBlob(trip.ledger, retained);
    return trip;
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
