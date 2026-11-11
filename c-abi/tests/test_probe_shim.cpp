// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The PROBE SHIM, driven through its export table exactly as a binding drives
// the shipped one (D-BIND-58, D-BIND-61, D-BIND-62).
//
// This executable links `fletcher-c-abi-probe` and NOT `fletcher-c-abi`: one
// shim per process, which is also what D-BIND-17's single-copy check enforces.
// It proves the instrument at the C level before any binding relies on it:
//
//   * the fused publish (`fl_publisher_publish_row`) scores encode_copies == 0,
//     its payload appended from the caller's own Arrow buffer;
//   * the two controls D-BIND-61 names score exactly 1 - a STAGED row pushed
//     through `fl_publisher_publish_raw`, and a COPIED EXPORT;
//   * a loaned attachment crosses by address;
//   * the scoring entry points refuse a provider that is not the probe.
//
// The row is one non-null binary field, so its encoding is
// `[bitfield:1][len:u32][payload]` and the payload is the one run of bytes with
// a provenance to trace (scalars are appended from stack temporaries).

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fletcher/abi/binding.h"
#include "fletcher/abi/test/probe.h"

namespace {

std::string MessageOf(const fl_error& err) {
    if (err.message == nullptr) return {};
    return {reinterpret_cast<const char*>(err.message), err.message_len};
}

fl_str Str(const char* s) { return {reinterpret_cast<const uint8_t*>(s), std::strlen(s)}; }

std::shared_ptr<arrow::Schema> PayloadSchema() {
    return arrow::schema({arrow::field("payload", arrow::binary(), /*nullable=*/false)});
}

/// One row whose payload is a deterministic pattern of `len` bytes.
std::shared_ptr<arrow::RecordBatch> OneRow(size_t len, uint8_t salt = 0) {
    std::string bytes(len, '\0');
    for (size_t i = 0; i < len; ++i) bytes[i] = static_cast<char>((i * 31u + 7u + salt) & 0xFFu);
    arrow::BinaryBuilder builder;
    EXPECT_TRUE(builder.Append(bytes).ok());
    std::shared_ptr<arrow::Array> array;
    EXPECT_TRUE(builder.Finish(&array).ok());
    return arrow::RecordBatch::Make(PayloadSchema(), 1, {array});
}

/// The payload's bytes where the CALLER holds them - the address the codec
/// must append from for the publish to be zero-copy.
const uint8_t* PayloadOf(const arrow::RecordBatch& batch) {
    const auto& binary = static_cast<const arrow::BinaryArray&>(*batch.column(0));
    return binary.value_data()->data() + binary.value_offset(0);
}

/// What the subscriber saw, sampled inside the delivery.
struct Seen {
    int deliveries = 0;
    std::vector<uint8_t> row_bytes;
    fl_test_delivery delivery = {};
};

void OnDelivery(void* ctx, uint64_t, const uint8_t* data, size_t len, const fl_schema*,
                const fl_attachments* atts) {
    auto* seen = static_cast<Seen*>(ctx);
    ++seen->deliveries;
    seen->delivery.row = data;
    seen->delivery.row_len = len;
    seen->row_bytes.assign(data, data + len);
    fl_blob loaned = {};
    if (atts != nullptr && fl_attachments_find(atts, Str("loaned"), &loaned) == 1) {
        seen->delivery.loaned = loaned.data;
        seen->delivery.loaned_len = loaned.size;
    }
}

/// A provider, a publisher and one subscription on one topic, torn down in order.
class ProbeFixture : public ::testing::Test {
   protected:
    void SetUp() override { Open("probe"); }

    void Open(const char* selector) {
        fl_error err = {};
        fl_provider_config config = {};
        ASSERT_EQ(fl_provider_create(Str(selector), &config, &provider_, &err), FL_OK)
            << MessageOf(err);
        ASSERT_EQ(fl_publisher_create(provider_, &publisher_, &err), FL_OK) << MessageOf(err);
        ASSERT_EQ(fl_subscriber_create(provider_, &subscriber_, &err), FL_OK) << MessageOf(err);

        ArrowSchema schema = {};
        ASSERT_TRUE(arrow::ExportSchema(*PayloadSchema(), &schema).ok());
        ASSERT_EQ(fl_publisher_create_topic(publisher_, Topic(), &schema, &err), FL_OK)
            << MessageOf(err);
        ASSERT_EQ(fl_codec_open(&schema, &codec_, &err), FL_OK) << MessageOf(err);
        schema.release(&schema);

        fl_schema_arrival* arrival = nullptr;
        ASSERT_EQ(fl_subscriber_subscribe(subscriber_, Topic(), &OnDelivery, &seen_, &sub_id_,
                                          &arrival, &err),
                  FL_OK)
            << MessageOf(err);
        fl_schema_arrival_dispose(arrival);
    }

    void TearDown() override {
        if (codec_ != nullptr) fl_codec_close(codec_);
        if (subscriber_ != nullptr) fl_subscriber_destroy(subscriber_);
        if (publisher_ != nullptr) fl_publisher_destroy(publisher_);
        if (provider_ != nullptr) fl_provider_destroy(provider_);
    }

    fl_topic Topic() const { return {segments_, 2}; }

    /// Publish row 0 of `batch` through THE FUSED PATH.
    void PublishFused(const arrow::RecordBatch& batch) {
        ArrowArray array = {};
        ASSERT_TRUE(arrow::ExportRecordBatch(batch, &array).ok());
        fl_error err = {};
        fl_rows* rows = nullptr;
        ASSERT_EQ(fl_rows_bind(codec_, &array, &rows, &err), FL_OK) << MessageOf(err);
        EXPECT_EQ(fl_publisher_publish_row(publisher_, Topic(), rows, 0, nullptr, &err), FL_OK)
            << MessageOf(err);
        fl_rows_unbind(rows);
        array.release(&array);
    }

    fl_test_verdict Score(const uint8_t* payload, size_t len) {
        fl_test_verdict verdict = {};
        fl_error err = {};
        EXPECT_EQ(fl_test_probe_score(provider_, payload, len, &seen_.delivery, &verdict, &err),
                  FL_OK)
            << MessageOf(err);
        fl_error_dispose(&err);
        return verdict;
    }

    const fl_str segments_[2] = {Str("bind5"), Str("probe")};
    fl_provider* provider_ = nullptr;
    fl_publisher* publisher_ = nullptr;
    fl_subscriber* subscriber_ = nullptr;
    fl_codec* codec_ = nullptr;
    uint64_t sub_id_ = 0;
    Seen seen_;
};

constexpr size_t kPayload = 59;  // with the 5-byte header, a 64-byte row

TEST_F(ProbeFixture, TheFusedPublishAppendsThePayloadFromTheCallersBuffer) {
    const auto batch = OneRow(kPayload);
    PublishFused(*batch);
    ASSERT_EQ(seen_.deliveries, 1);

    const fl_test_verdict verdict = Score(PayloadOf(*batch), kPayload);
    EXPECT_EQ(verdict.encode_copies, 0);
    EXPECT_EQ(verdict.produced_at, reinterpret_cast<uintptr_t>(PayloadOf(*batch)));
    EXPECT_EQ(verdict.row_copies, 0u);
    EXPECT_EQ(verdict.encode_len, kPayload + 5u);
}

/// D-BIND-61's staging control: the same row, composed before the publish and
/// handed over whole. `row_copies` cannot see it - only the source can.
TEST_F(ProbeFixture, AStagedRowPushedThroughARawPublishIsACopy) {
    const auto batch = OneRow(kPayload);
    PublishFused(*batch);
    const std::vector<uint8_t> staged = seen_.row_bytes;

    fl_error err = {};
    auto writer = [](void* ctx, uint8_t* dst, size_t room) -> size_t {
        const auto* row = static_cast<const std::vector<uint8_t>*>(ctx);
        if (room < row->size()) return 0;
        std::memcpy(dst, row->data(), row->size());
        return row->size();
    };
    ASSERT_EQ(fl_publisher_publish_raw(publisher_, Topic(), writer,
                                       const_cast<std::vector<uint8_t>*>(&staged), staged.size(),
                                       nullptr, &err),
              FL_OK)
        << MessageOf(err);

    const fl_test_verdict verdict = Score(PayloadOf(*batch), kPayload);
    EXPECT_EQ(verdict.encode_copies, 1);
    EXPECT_EQ(verdict.row_copies, 0u) << "the provider half is clean: the copy was the client's";
}

/// D-BIND-61's copied-export control: the payload's bytes at a SECOND address,
/// identical in content, so only provenance can tell.
TEST_F(ProbeFixture, AnExportThatCopiedItsBuffersIsACopy) {
    const auto original = OneRow(kPayload);
    const auto copied = OneRow(kPayload);
    ASSERT_NE(PayloadOf(*original), PayloadOf(*copied));

    PublishFused(*copied);

    const fl_test_verdict verdict = Score(PayloadOf(*original), kPayload);
    EXPECT_EQ(verdict.encode_copies, 1);
    EXPECT_EQ(verdict.produced_at, reinterpret_cast<uintptr_t>(PayloadOf(*copied)))
        << "the verdict names where the bytes really came from";
}

TEST_F(ProbeFixture, ALoanedAttachmentIsDeliveredByAddress) {
    std::vector<uint8_t> loan(1024);
    for (size_t i = 0; i < loan.size(); ++i) loan[i] = static_cast<uint8_t>(i);
    uintptr_t base = 0;
    fl_error err = {};
    ASSERT_EQ(fl_test_probe_loan(provider_, Str("loaned"), loan.data(), loan.size(), &base, &err),
              FL_OK)
        << MessageOf(err);

    const auto batch = OneRow(kPayload);
    PublishFused(*batch);

    ASSERT_EQ(reinterpret_cast<uintptr_t>(seen_.delivery.loaned), base);
    const fl_test_verdict verdict = Score(PayloadOf(*batch), kPayload);
    EXPECT_EQ(verdict.attachment_copies, 0u);
    EXPECT_EQ(verdict.encode_copies, 0);
}

/// A delivery that dropped the loaned attachment is a COPY, not a pass.
TEST_F(ProbeFixture, AMissingLoanedAttachmentScoresAsACopy) {
    std::vector<uint8_t> loan(64, 0x11);
    uintptr_t base = 0;
    fl_error err = {};
    ASSERT_EQ(fl_test_probe_loan(provider_, Str("loaned"), loan.data(), loan.size(), &base, &err),
              FL_OK);
    const auto batch = OneRow(kPayload);
    PublishFused(*batch);
    seen_.delivery.loaned = nullptr;
    seen_.delivery.loaned_len = 0;

    EXPECT_EQ(Score(PayloadOf(*batch), kPayload).attachment_copies, 1u);
}

TEST_F(ProbeFixture, APayloadNotInTheRowIsRefusedNotScored) {
    const auto batch = OneRow(kPayload);
    PublishFused(*batch);
    const auto other = OneRow(kPayload, /*salt=*/99);

    fl_test_verdict verdict = {};
    fl_error err = {};
    EXPECT_EQ(fl_test_probe_score(provider_, PayloadOf(*other), kPayload, &seen_.delivery, &verdict,
                                  &err),
              FL_INVALID_ARGUMENT);
    EXPECT_NE(MessageOf(err).find("not in the published row"), std::string::npos) << MessageOf(err);
    fl_error_dispose(&err);
}

TEST(ProbeShim, ScoringAProviderThatIsNotTheProbeIsRefused) {
    fl_error err = {};
    fl_provider_config config = {};
    fl_provider* provider = nullptr;
    ASSERT_EQ(fl_provider_create(Str("inprocess"), &config, &provider, &err), FL_OK);

    fl_test_delivery delivery = {};
    fl_test_verdict verdict = {};
    const uint8_t byte = 0;
    EXPECT_EQ(fl_test_probe_score(provider, &byte, 1, &delivery, &verdict, &err),
              FL_INVALID_ARGUMENT);
    EXPECT_NE(MessageOf(err).find("'probe'"), std::string::npos) << MessageOf(err);
    fl_error_dispose(&err);
    fl_provider_destroy(provider);
}

}  // namespace
