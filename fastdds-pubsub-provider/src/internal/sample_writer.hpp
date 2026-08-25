// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The two ways DDS lets a DataWriter accept a sample, one class each — the publish-side counterpart
// of internal/data_reader_listener.hpp:
//
//   LoanableSampleWriter  loan_sample() → fill FletcherSample → write(sample). Fast DDS skips
//                         serialize() entirely for a loaned plain sample, and with data-sharing the
//                         struct being filled *is* the one the reader reads, so nothing is copied
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
        // A row past the provider's payload bound fails inside serialize() and Fast DDS drops the
        // sample. This path cannot throw the way the loaned one does: the failure happens
        // underneath write() rather than in front of it. It must not be silent either.
        if (writer->write(&transport) != eprosima::fastdds::dds::RETCODE_OK) {
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "writer on '"
                                   << writer->get_topic()->get_name()
                                   << "' dropped a sample: write failed, most likely a row "
                                      "larger than the provider's payload bound");
        }
    }
};

// N is the provider's payload bound: the loan is a FletcherSample<N>, so this class only fits a
// writer whose registered type carries the same N. The provider instantiates the two together.
template <uint32_t N>
class LoanableSampleWriter : public SampleWriterBase {
   public:
    void Write(eprosima::fastdds::dds::DataWriter* writer,
               const PubSubProvider::RowEncoder& encoder, const Attachments& attachments) override {
        void* sample = nullptr;
        if (writer->loan_sample(sample,
                                eprosima::fastdds::dds::DataWriter::LoanInitializationKind::
                                    NO_LOAN_INITIALIZATION) != eprosima::fastdds::dds::RETCODE_OK) {
            // Loan pool exhausted. Falling back keeps the publish correct — same bytes, one copy —
            // rather than dropping the sample, and nothing reports the degradation.
            fallback_.Write(writer, encoder, attachments);
            return;
        }

        auto* fletcher_sample = static_cast<FletcherSample<N>*>(sample);
        try {
            FixedWriteBuffer buf(fletcher_sample->body, N);
            EncodeEnvelopeBody(buf, encoder, attachments);
            fletcher_sample->length = static_cast<uint32_t>(buf.Position());
        } catch (...) {
            writer->discard_loan(sample);
            throw;
        }

        // On failure Fast DDS re-registers the loan and returns without publishing
        // (DataWriterImpl.cpp), so dropping the pointer here would cost the writer a loan slot
        // permanently — and there are only max_samples + extra_samples of them.
        if (writer->write(sample) != eprosima::fastdds::dds::RETCODE_OK) {
            writer->discard_loan(sample);
        }
    }

   private:
    SampleWriter fallback_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SAMPLE_WRITER_HPP_
