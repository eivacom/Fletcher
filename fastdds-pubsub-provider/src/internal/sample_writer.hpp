// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The two ways DDS lets a DataWriter accept a sample, one class each — the publish-side counterpart
// of internal/data_reader_listener.hpp:
//
//   LoanableSampleWriter  loan_sample() → fill the sample → write(sample). Fast DDS skips
//                         serialize() entirely for a loaned plain sample, and with data-sharing the
//                         payload being filled *is* the one the reader reads, so nothing is copied
//                         at any point.
//   SampleWriter          write(&PublishData) → FletcherSamplePubSubType::serialize writes the same
//                         layout into the transport's payload, truncated after the bytes in use.
//
// Both derive from SampleWriterBase, and both leave the same fields in the same layout, so a reader
// cannot tell which one produced a sample. Unlike the read flows, this is a genuine preference
// rather than a precondition — see FastDDSProviderOptions::loan_publish for what it costs.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_

#include <cstdint>
#include <fastdds/dds/core/ReturnCode.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <utility>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

class SampleWriterBase {
   public:
    virtual ~SampleWriterBase() = default;

    // Encodes one row and hands it to `writer`. Called under the provider's shared lock, so it must
    // not touch provider state; `writer` is kept alive by that lock for the duration of the call.
    virtual void Write(eprosima::fastdds::dds::DataWriter* writer,
                       const PubSubProvider::RowEncoder& encoder,
                       const Attachments& attachments) = 0;
};

class SampleWriter : public SampleWriterBase {
   public:
    void Write(eprosima::fastdds::dds::DataWriter* writer,
               const PubSubProvider::RowEncoder& encoder, const Attachments& attachments) override {
        // The encoder still writes row bytes straight into the DDS payload buffer, from inside
        // serialize() — the copy this path is named for is the transport's, not one of ours.
        PublishData transport;
        transport.encoder = &encoder;
        transport.attachments = &attachments;
        // An oversized row fails under write() rather than in front of it, so it cannot throw here.
        const eprosima::fastdds::dds::ReturnCode_t rc = writer->write(&transport);
        if (rc != eprosima::fastdds::dds::RETCODE_OK) {
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "writer on '" << writer->get_topic()->get_name()
                                             << "' dropped a sample, return code " << rc);
        }
    }
};

// Only fits a writer whose registered type carries the same bound.
class LoanableSampleWriter : public SampleWriterBase {
   public:
    explicit LoanableSampleWriter(uint32_t payload_bytes) : payload_bytes_(payload_bytes) {}

    void Write(eprosima::fastdds::dds::DataWriter* writer,
               const PubSubProvider::RowEncoder& encoder, const Attachments& attachments) override {
        void* sample = nullptr;
        const eprosima::fastdds::dds::ReturnCode_t loan = writer->loan_sample(
            sample,
            eprosima::fastdds::dds::DataWriter::LoanInitializationKind::NO_LOAN_INITIALIZATION);
        if (loan != eprosima::fastdds::dds::RETCODE_OK) {
            // A failed loan drops the sample: loan_publish asked for zero copy.
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
            writer->discard_loan(sample);
            throw;
        }

        // On failure Fast DDS re-registers the loan and returns without publishing
        // (DataWriterImpl.cpp), so dropping the pointer here would cost the writer a loan slot
        // permanently — and there are only max_samples + extra_samples of them.
        const eprosima::fastdds::dds::ReturnCode_t rc = writer->write(sample);
        if (rc != eprosima::fastdds::dds::RETCODE_OK) {
            writer->discard_loan(sample);
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "writer on '" << writer->get_topic()->get_name()
                                             << "' dropped a loaned sample, return code " << rc);
        }
    }

   private:
    uint32_t payload_bytes_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_
