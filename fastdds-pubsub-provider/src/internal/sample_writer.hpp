// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The two ways a DataWriter accepts a sample. `Publish` uses WriteSample:
// write(&PublishData) → FletcherSamplePubSubType::serialize writes the envelope into the
// transport's payload, truncated after the bytes in use. LoanableSampleWriter (loan_sample →
// fill → write(sample)) is kept and unit-tested but not selected: the regular path already
// writes straight into the DDS payload buffer, and a loaned write ships the full bound on the
// wire regardless of the row's real size. Both leave the same layout, so a reader cannot tell
// which one produced a sample.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_

#include <cstdint>
#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <stdexcept>
#include <string>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// Encodes one row and hands it to `writer`. Called under the provider's shared lock, so it must
// not touch provider state; `writer` is kept alive by that lock for the duration of the call.
inline void WriteSample(eprosima::fastdds::dds::DataWriter* writer,
                        const PubSubProvider::RowEncoder& encoder, const Attachments& attachments) {
    // The encoder still writes row bytes straight into the DDS payload buffer, from inside
    // serialize() — the copy this path is named for is the transport's, not one of ours.
    PublishData transport;
    transport.encoder = &encoder;
    transport.attachments = &attachments;
    // An oversized row fails under write() rather than in front of it, so it cannot throw here.
    const eprosima::fastdds::dds::ReturnCode_t rc = writer->write(&transport);

    // write() serializes into the history before it returns in EVERY publish mode
    // (DataWriterImpl::perform_create_new_change calls type_->serialize before add_pub_change;
    // ASYNCHRONOUS_PUBLISH_MODE only hands the finished change to the flow controller), so a
    // failed encode has already recorded its reason by now and `transport` may live on the
    // stack. Fast DDS does not propagate a serialize failure into write()'s return code, and
    // rc != OK is itself an ordinary outcome (no matched reader, backpressure) — so the
    // diagnostic, not rc, is the reliable signal that THIS row failed to encode. Checked first
    // so the caller gets the cause rather than a bare return code.
    if (!transport.serialize_error.empty()) {
        // kInternal, NOT kTransportFailure: the only thing recorded here is the caller's encoder
        // throwing, and the transport is blameless. A binding that retries kTransportFailure must
        // not retry it.
        //
        // The one asymmetry left is deliberate and test-pinned: an OVERSIZED row throws
        // kPayloadTooLarge on the loaned flow (which encodes in front of write()) and is
        // dropped and logged here (see serialize()'s overflow catch and
        // FastDDSPubSubProviderTest.DataSharingOversizedRowDoesNotThrow).
        throw PubSubError(PubSubStatus::kInternal, "FastDDS: failed to publish to '" +
                                                       writer->get_topic()->get_name() +
                                                       "': " + transport.serialize_error);
    }
    if (rc != eprosima::fastdds::dds::RETCODE_OK) {
        EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION, "writer on '" << writer->get_topic()->get_name()
                                                               << "' dropped a sample, return code "
                                                               << rc);
    }
}

// Not selected: `Publish` always writes through `WriteSample`. The regular path already writes
// straight into the DDS payload buffer, from inside serialize(); a loaned write ships the full
// bound on the wire regardless of the row's real size.
//
// Only fits a writer whose registered type carries the same bound.
class LoanableSampleWriter {
   public:
    explicit LoanableSampleWriter(uint32_t payload_bytes) : payload_bytes_(payload_bytes) {}

    void Write(eprosima::fastdds::dds::DataWriter* writer,
               const PubSubProvider::RowEncoder& encoder, const Attachments& attachments) {
        void* sample = nullptr;
        const eprosima::fastdds::dds::ReturnCode_t loan = writer->loan_sample(
            sample,
            eprosima::fastdds::dds::DataWriter::LoanInitializationKind::NO_LOAN_INITIALIZATION);
        if (loan != eprosima::fastdds::dds::RETCODE_OK) {
            // A failed loan drops the sample: a caller of this class asked for zero copy.
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "writer on '" << writer->get_topic()->get_name()
                                             << "' dropped a sample: loan_sample returned "
                                             << loan);
            return;
        }

        auto* bytes = static_cast<uint8_t*>(sample);
        try {
            FixedWriteBuffer buf(SampleBody(bytes), payload_bytes_);
            EncodeEnvelopeBody(buf, encoder, attachments);
            WriteSampleLength(bytes, static_cast<uint32_t>(buf.Position()));
        } catch (...) {
            // Encoding happens in front of write() on this path, so the caller is already inside
            // Publish and the exception reaches it by simply propagating — no diagnostic to record
            // and read back (contrast WriteSample above). Propagated AS THROWN, deliberately: an
            // oversized row surfaces as the std::overflow_error FixedWriteBuffer raised, which is
            // what distinguishes "did not fit the bound" from an encoder defect.
            const eprosima::fastdds::dds::ReturnCode_t discard_rc = writer->discard_loan(sample);
            if (discard_rc != eprosima::fastdds::dds::RETCODE_OK) {
                EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                                   "writer on '" << writer->get_topic()->get_name()
                                                 << "' discard_loan returned " << discard_rc
                                                 << " while unwinding an encoding failure");
            }
            throw;
        }

        // On failure Fast DDS re-registers the loan only for RETCODE_TIMEOUT (`add_pub_change`
        // failed); for RETCODE_OUT_OF_RESOURCES (`create_change` failed) the loan record is
        // already gone, so `discard_loan` below returns RETCODE_BAD_PARAMETER instead.
        const eprosima::fastdds::dds::ReturnCode_t rc = writer->write(sample);
        if (rc != eprosima::fastdds::dds::RETCODE_OK) {
            const eprosima::fastdds::dds::ReturnCode_t discard_rc = writer->discard_loan(sample);
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "writer on '" << writer->get_topic()->get_name()
                                             << "' dropped a loaned sample, return code " << rc);
            if (discard_rc != eprosima::fastdds::dds::RETCODE_OK) {
                EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                                   "writer on '" << writer->get_topic()->get_name()
                                                 << "' discard_loan returned " << discard_rc
                                                 << " after the dropped write");
            }
        }
    }

   private:
    uint32_t payload_bytes_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_
