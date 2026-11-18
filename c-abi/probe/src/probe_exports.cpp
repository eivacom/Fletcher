// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The probe shim's `fl_test_*` entry points (`probe/include/fletcher/abi/test/
// probe.h`). Compiled into `fletcher-c-abi-probe` only.
//
// They do no scoring of their own: the ledger is filled from what the probe
// recorded and what the binding's subscriber reported, and `Judge()` - the copy
// oracle's one scoring function, from `fletcher-copy-probe` - decides. A binding
// leg therefore faces the same numbers as every C++ leg.
#include <fletcher/copy_probe/ledger.hpp>
#include <fletcher/copy_probe/seam_probe_provider.hpp>
#include <fletcher/copy_probe/tracing_window.hpp>
#include <fletcher/core/status.hpp>
#include <string>
#include <vector>

#include "containment.hpp"
#include "fletcher/abi/test/probe.h"
#include "handles.hpp"

namespace {

using fletcher::PubSubError;
using fletcher::PubSubStatus;
using fletcher::abi::Contain;
using fletcher::copy_probe::At;
using fletcher::copy_probe::AttachmentTrace;
using fletcher::copy_probe::CopyLedger;
using fletcher::copy_probe::CopyVerdict;
using fletcher::copy_probe::SeamProbeProvider;

/// The provider behind a handle, as the probe - or a refusal naming the entry
/// point, since the one mistake a caller can make here is to pass a provider
/// created from any selector but `probe`.
SeamProbeProvider& ProbeOf(fl_provider* provider, const char* entry) {
    auto* probe =
        provider == nullptr ? nullptr : dynamic_cast<SeamProbeProvider*>(provider->provider.get());
    if (probe == nullptr) {
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            std::string(entry) + ": the provider was not created from the selector 'probe'");
    }
    return *probe;
}

}  // namespace

fl_status fl_test_probe_loan(fl_provider* provider, fl_str key, const uint8_t* bytes, size_t len,
                             uintptr_t* base, fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (base == nullptr || (bytes == nullptr && len != 0) || key.data == nullptr ||
            key.len == 0) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_test_probe_loan: key, bytes and base must be given");
        }
        SeamProbeProvider& probe = ProbeOf(provider, "fl_test_probe_loan");
        *base =
            At(probe.LoanForDelivery(std::string(reinterpret_cast<const char*>(key.data), key.len),
                                     std::vector<uint8_t>(bytes, bytes + len)));
    });
}

fl_status fl_test_probe_score(fl_provider* provider, const uint8_t* payload, size_t payload_len,
                              const fl_test_delivery* delivery, fl_test_verdict* out,
                              fl_error* err) {
    return Contain(err, FL_ORIGIN_SEAM, [&] {
        if (delivery == nullptr || out == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "fl_test_probe_score: delivery and out must be given");
        }
        SeamProbeProvider& probe = ProbeOf(provider, "fl_test_probe_score");

        CopyLedger ledger;
        const std::string unscorable = fletcher::copy_probe::ScoreProducedFromSource(
            probe.LastTrace(), payload, payload_len, ledger);
        if (!unscorable.empty()) {
            throw PubSubError(PubSubStatus::kInvalidArgument, "fl_test_probe_score: " + unscorable);
        }

        ledger.deliveries = 1;
        ledger.delivered_data = At(delivery->row);
        ledger.delivered_len = delivery->row_len;
        if (probe.LoanedLen() > 0) {
            AttachmentTrace loaned;
            loaned.key = probe.LoanedKey();
            loaned.published_data = At(probe.LoanedBase());
            loaned.published_len = probe.LoanedLen();
            loaned.delivered_data = At(delivery->loaned);
            loaned.delivered_len = delivery->loaned_len;
            ledger.attachments.push_back(std::move(loaned));
        }

        const CopyVerdict verdict = fletcher::copy_probe::Judge(ledger);
        out->encode_copies =
            verdict.encode_copies.has_value() ? static_cast<int64_t>(*verdict.encode_copies) : -1;
        out->row_copies = verdict.row_copies;
        out->attachment_copies = verdict.attachment_copies;
        out->produced_at = ledger.produced_at;
        out->encode_base = ledger.encode_base;
        out->encode_len = ledger.encode_len;
    });
}
