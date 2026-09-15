// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The data reader side is a Fast DDS DataReaderListener again (owner decision 2026-09-15, the
// "hybrid" round -- see the file-header table and Impl::HandleSchema in
// fast_dds_pubsub_provider.cpp). A listener's on_data_available runs INSIDE Fast DDS's own accept
// path (StatefulWriter::deliver_sample_to_intraprocesses -> DataReaderImpl::process_data_msg), so
// a same-process reader's history can never fill out from under an asynchronous consumer the way
// it could behind a polling WaitSet thread -- that gap is what dropped samples on a two-core run
// this week (see the file-header table). Every reader (schema and data) still starts DISABLED
// (SubscriberQos::entity_factory) and is enabled only once its topic's schema is known -- the data
// reader by Subscribe if the schema is already there, else by this provider's one schema thread --
// so `Drain` below is never reached before `SetSchema` has run.
//
// This is the read-side counterpart of internal/sample_writer.hpp.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_

#include <cassert>
#include <cstdint>
#include <fastdds/dds/core/LoanableSequence.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/TopicDescription.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/delivery_channel.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <utility>
#include <vector>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "status_endpoint.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// Only PREALLOCATED* pools give whole-sample nodes; DYNAMIC ones size each to what arrived.
inline bool CanLoanSamples(const eprosima::fastdds::dds::DataReaderQos& qos) {
    const auto policy = qos.endpoint().history_memory_policy;
    return policy == eprosima::fastdds::rtps::PREALLOCATED_MEMORY_MODE ||
           policy == eprosima::fastdds::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
}

// One per subscribed topic, installed on that topic's data DataReader at creation
// (`create_datareader(ts.topic, rqos, ts.listener.get(), StatusMask::all())`,
// fast_dds_pubsub_provider.cpp). The reader is created DISABLED, so nothing here runs before
// `enable()` does. Forwards every status Fast DDS has for a reader -- the mapping the deleted
// standalone DataReaderStatusListener class (and, before that, Impl::DispatchReaderStatuses) used.
// `on_data_available` is `final` so no override can skip the try/catch that keeps a throwing
// `Drain` off Fast DDS's own delivery thread.
class DataReaderListenerBase : public eprosima::fastdds::dds::DataReaderListener {
   public:
    explicit DataReaderListenerBase(FastDDSStatusListener* status_listener)
        : status_listener_(status_listener) {}

    // Called exactly once, by whichever thread is about to `enable()` this reader (Subscribe, or
    // this provider's schema thread once the schema arrives) -- always before that `enable()`, so
    // `Drain` never runs with `schema_` unset (asserted there).
    void SetSchema(SharedSchema schema) { schema_ = std::move(schema); }

    void on_data_available(eprosima::fastdds::dds::DataReader* reader) final {
        try {
            Drain(reader);
        } catch (const std::exception& ex) {
            EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION, "reading a sample threw: " << ex.what());
        } catch (...) {
            EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION, "reading a sample threw a non-std exception");
        }
    }

    void on_subscription_matched(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::SubscriptionMatchedStatus& info) override {
        if (status_listener_)
            status_listener_->OnMatched(ReaderEndpoint(reader), info.current_count,
                                        info.current_count_change);
    }

    void on_requested_deadline_missed(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::RequestedDeadlineMissedStatus& status) override {
        if (status_listener_)
            status_listener_->OnDeadlineMissed(ReaderEndpoint(reader), status.total_count);
    }

    void on_liveliness_changed(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::LivelinessChangedStatus& status) override {
        if (status_listener_)
            status_listener_->OnLivelinessChanged(ReaderEndpoint(reader), status.alive_count,
                                                  status.not_alive_count);
    }

    void on_requested_incompatible_qos(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::RequestedIncompatibleQosStatus& status) override {
        if (status_listener_)
            status_listener_->OnIncompatibleQos(ReaderEndpoint(reader),
                                                static_cast<uint32_t>(status.last_policy_id),
                                                status.total_count);
    }

    void on_sample_rejected(eprosima::fastdds::dds::DataReader* reader,
                            const eprosima::fastdds::dds::SampleRejectedStatus& status) override {
        if (status_listener_)
            status_listener_->OnSampleRejected(ReaderEndpoint(reader),
                                               static_cast<int32_t>(status.last_reason),
                                               status.total_count);
    }

    void on_sample_lost(eprosima::fastdds::dds::DataReader* reader,
                        const eprosima::fastdds::dds::SampleLostStatus& status) override {
        if (status_listener_)
            status_listener_->OnSampleLost(ReaderEndpoint(reader),
                                           static_cast<uint32_t>(status.total_count));
    }

    virtual void Drain(eprosima::fastdds::dds::DataReader* reader) = 0;

   protected:
    // Never null once `Drain` can run (see `SetSchema` above); every `Drain` override asserts it.
    SharedSchema schema_;

   private:
    FastDDSStatusListener* status_listener_;
};

// Zero-copy read: samples reach the callback in the payloads Fast DDS already holds. Compiled and
// unit-tested (see the tests file), but NOT installed by Subscribe -- owner ruling keeps
// CopyingDataReaderListener as the listener Subscribe builds (fast_dds_pubsub_provider.cpp, the
// `ts.listener = std::make_unique<internal::CopyingDataReaderListener>(...)` line); this stays
// behind CanLoanSamples(rqos), which is the precondition a caller who does select it must still
// check.
class LoanedDataReaderListener : public DataReaderListenerBase {
   public:
    LoanedDataReaderListener(FastDDSStatusListener* status_listener, uint32_t payload_bytes,
                             DeliveryChannel channel)
        : DataReaderListenerBase(status_listener),
          payload_bytes_(payload_bytes),
          channel_(std::move(channel)) {}

   private:
    // A byte element: the collection holds payload pointers, and sizeof is only used by resize().
    FASTDDS_CONST_SEQUENCE(SampleSeq, uint8_t);

    void Drain(eprosima::fastdds::dds::DataReader* reader) override {
        assert(schema_);
        SampleSeq samples;
        eprosima::fastdds::dds::SampleInfoSeq infos;
        // Reused across samples within this one Drain call: ParseEnvelopeBody's bulk builder is
        // what empties it, on entry and on every early return.
        Attachments attachments;
        // Pre-sizing would silently switch this to a deserialising take into 1-byte elements.
        assert(samples.maximum() == 0);
        eprosima::fastdds::dds::ReturnCode_t rc;
        while ((rc = reader->take(samples, infos)) == eprosima::fastdds::dds::RETCODE_OK) {
            // ~LoanableSequence only warns, and a leaked loan costs a payload slot for good. Still
            // runs on the early `continue`s below: this batch's loan is returned either way.
            LoanReturn loan_return{reader, samples, infos};
            for (eprosima::fastdds::dds::LoanableCollection::size_type i = 0; i < samples.length();
                 ++i) {
                if (!infos[i].valid_data) continue;
                const uint8_t* sample = &samples[i];
                // A lapping writer can overwrite the slot mid-read.
                if (!reader->is_sample_valid(sample, &infos[i])) {
                    EPROSIMA_LOG_WARNING(
                        FLETCHER_SUBSCRIPTION,
                        "reader on '" << reader->get_topicdescription()->get_name()
                                      << "' dropped a sample: the writer lapped it before it was "
                                         "read");
                    continue;
                }
                const uint32_t length = ReadSampleLength(sample);
                if (length > payload_bytes_) {
                    EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                                         "reader on '" << reader->get_topicdescription()->get_name()
                                                       << "' dropped a sample: its length "
                                                       << length << " exceeds the payload bound "
                                                       << payload_bytes_);
                    continue;
                }
                const uint8_t* row = nullptr;
                uint32_t row_len = 0;
                const uint8_t* body = SampleBody(sample);

                // Attachments cannot alias the loaned payload beyond this call, so a sample that
                // carries any costs ONE owning copy of its body. A sample with none -- the hot
                // path -- is untouched: no owner, no copy, the row delivered where it lies.
                std::shared_ptr<const std::vector<uint8_t>> owned;
                if (PeekAttachmentCount(body, length) > 0) {
                    owned = std::make_shared<const std::vector<uint8_t>>(body, body + length);
                    body = owned->data();
                }

                if (!ParseEnvelopeBody(owned, body, length, row, row_len, attachments)) {
                    EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                                         "reader on '" << reader->get_topicdescription()->get_name()
                                                       << "' dropped a sample: malformed envelope");
                    continue;
                }
                if (!reader->is_sample_valid(sample, &infos[i])) {
                    EPROSIMA_LOG_WARNING(
                        FLETCHER_SUBSCRIPTION,
                        "reader on '" << reader->get_topicdescription()->get_name()
                                      << "' dropped a sample: the writer lapped it while it was "
                                         "being parsed");
                    continue;
                }
                channel_.Deliver(row, row_len, schema_, attachments);
            }
        }
        // Not retried: looping on a sample Fast DDS may not have consumed would spin this thread.
        if (rc != eprosima::fastdds::dds::RETCODE_NO_DATA) {
            EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                                 "reader on '" << reader->get_topicdescription()->get_name()
                                               << "' take failed with return code " << rc
                                               << "; the rest of this notification was not read");
        }
    }

    uint32_t payload_bytes_;
    DeliveryChannel channel_;

    struct LoanReturn {
        eprosima::fastdds::dds::DataReader* reader;
        SampleSeq& samples;
        eprosima::fastdds::dds::SampleInfoSeq& infos;
        ~LoanReturn() {
            // A leak costs a payload slot, and delete_datareader then refuses for good.
            if (reader->return_loan(samples, infos) != eprosima::fastdds::dds::RETCODE_OK) {
                EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                                   "return_loan failed; a payload slot is lost");
            }
        }
    };
};

// Copying read: deserialize bounds itself by payload.length, so short nodes are safe. The default
// listener Subscribe installs (owner decision 2026-09-14; see LoanedDataReaderListener's comment
// above).
class CopyingDataReaderListener : public DataReaderListenerBase {
   public:
    CopyingDataReaderListener(FastDDSStatusListener* status_listener, DeliveryChannel channel)
        : DataReaderListenerBase(status_listener), channel_(std::move(channel)) {}

   private:
    void Drain(eprosima::fastdds::dds::DataReader* reader) override {
        assert(schema_);
        ReceivedData data;
        eprosima::fastdds::dds::SampleInfo info;
        eprosima::fastdds::dds::ReturnCode_t rc;
        while ((rc = reader->take_next_sample(&data, &info)) ==
               eprosima::fastdds::dds::RETCODE_OK) {
            if (!info.valid_data) continue;
            channel_.Deliver(data.decoded_row.data(), data.decoded_row.size(), schema_,
                             data.decoded_attachments);
        }
        // Not retried: looping on a sample Fast DDS may not have consumed would spin this thread.
        if (rc != eprosima::fastdds::dds::RETCODE_NO_DATA) {
            EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                                 "reader on '" << reader->get_topicdescription()->get_name()
                                               << "' take_next_sample failed with return code "
                                               << rc
                                               << "; the rest of this notification was not read");
        }
    }

    DeliveryChannel channel_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_
