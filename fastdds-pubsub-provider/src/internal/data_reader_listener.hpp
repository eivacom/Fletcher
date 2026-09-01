// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The two read flows, both feeding OrderedDelivery; the schema arrives via SetSchema.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_

#include <cassert>
#include <cstdint>
#include <exception>
#include <fastdds/dds/core/LoanableSequence.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/TopicDescription.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <utility>
#include <vector>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "ordered_delivery.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// Only PREALLOCATED* pools give whole-sample nodes; DYNAMIC ones size each to what arrived.
inline bool CanLoanSamples(const eprosima::fastdds::dds::DataReaderQos& qos) {
    const auto policy = qos.endpoint().history_memory_policy;
    return policy == eprosima::fastdds::rtps::PREALLOCATED_MEMORY_MODE ||
           policy == eprosima::fastdds::rtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
}

// What both flows share: the delivery queue, the schema handoff, and the statuses worth logging.
class DataReaderListenerBase : public eprosima::fastdds::dds::DataReaderListener {
   public:
    // `max_queued` bounds the pre-schema backlog.
    DataReaderListenerBase(PubSubProvider::SubscribeCallback cb, SharedSchema schema,
                           size_t max_queued)
        : delivery_(std::move(cb), std::move(schema), max_queued) {}

    // Nothing may escape: OrderedDelivery rethrows user code, which would terminate the process.
    void on_data_available(eprosima::fastdds::dds::DataReader* reader) final {
        try {
            Take(reader);
        } catch (const std::exception& e) {
            EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION, "subscribe callback threw: " << e.what());
        } catch (...) {
            EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                               "subscribe callback threw a non-std exception");
        }
    }

    // INFO needs FASTDDS_ENFORCE_LOG_INFO to appear at all (Log.hpp).
    void on_subscription_matched(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::SubscriptionMatchedStatus& info) final {
        if (info.current_count_change < 0) {
            EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                                 "reader on '" << reader->get_topicdescription()->get_name()
                                               << "' lost a writer, " << info.current_count
                                               << " still matched");
        } else {
            EPROSIMA_LOG_INFO(FLETCHER_SUBSCRIPTION,
                              "reader on '" << reader->get_topicdescription()->get_name()
                                            << "' matched a writer, " << info.current_count
                                            << " now matched");
        }
    }

    // Only fires on a reader given a DEADLINE through FastDDSProviderOptions.
    void on_requested_deadline_missed(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::RequestedDeadlineMissedStatus& status) final {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << reader->get_topicdescription()->get_name()
                                           << "' missed its requested deadline, "
                                           << status.total_count << " times in all");
    }

    // Under AUTOMATIC with an infinite lease, not-alive means the writer vanished.
    void on_liveliness_changed(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::LivelinessChangedStatus& status) final {
        if (status.not_alive_count_change > 0) {
            EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                                 "reader on '" << reader->get_topicdescription()->get_name()
                                               << "' has " << status.not_alive_count
                                               << " writer(s) no longer asserting liveliness, "
                                               << status.alive_count << " still alive");
        } else {
            EPROSIMA_LOG_INFO(FLETCHER_SUBSCRIPTION,
                              "reader on '" << reader->get_topicdescription()->get_name()
                                            << "' has " << status.alive_count << " live writer(s)");
        }
    }

    // A mismatch leaves the subscriber unconnected forever.
    void on_requested_incompatible_qos(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::RequestedIncompatibleQosStatus& status) final {
        EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                           "reader on '" << reader->get_topicdescription()->get_name()
                                         << "' rejected by a writer over QoS policy id "
                                         << status.last_policy_id << "; no samples will arrive");
    }

    void on_sample_lost(eprosima::fastdds::dds::DataReader* reader,
                        const eprosima::fastdds::dds::SampleLostStatus& status) final {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << reader->get_topicdescription()->get_name()
                                           << "' lost " << status.total_count << " sample(s)");
    }

    void on_sample_rejected(eprosima::fastdds::dds::DataReader* reader,
                            const eprosima::fastdds::dds::SampleRejectedStatus& status) final {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << reader->get_topicdescription()->get_name()
                                           << "' rejected a sample (reason "
                                           << static_cast<int>(status.last_reason) << ", "
                                           << status.total_count
                                           << " total); resource limits are too tight");
    }

    // Delivers backlog then live samples in order, with the callback outside any provider lock.
    void SetSchema(SharedSchema schema) { delivery_.SetSchema(std::move(schema)); }

   protected:
    virtual void Take(eprosima::fastdds::dds::DataReader* reader) = 0;

    OrderedDelivery delivery_;
};

// Zero-copy read: samples reach the callback in the payloads Fast DDS already holds.
class LoanableDataReaderListener : public DataReaderListenerBase {
   public:
    LoanableDataReaderListener(uint32_t payload_bytes, PubSubProvider::SubscribeCallback cb,
                               SharedSchema schema, size_t max_queued)
        : DataReaderListenerBase(std::move(cb), std::move(schema), max_queued),
          payload_bytes_(payload_bytes) {}

   private:
    // A byte element: the collection holds payload pointers, and sizeof is only used by resize().
    FASTDDS_CONST_SEQUENCE(SampleSeq, uint8_t);

    void Take(eprosima::fastdds::dds::DataReader* reader) override {
        SampleSeq samples;
        eprosima::fastdds::dds::SampleInfoSeq infos;
        // Reused: a fresh empty unordered_map costs 51 ns on MSVC.
        Attachments attachments;
        // Pre-sizing would silently switch this to a deserialising take into 1-byte elements.
        assert(samples.maximum() == 0);
        while (reader->take(samples, infos) == eprosima::fastdds::dds::RETCODE_OK) {
            // ~LoanableSequence only warns, and a leaked loan costs a payload slot for good.
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

                // The loan is returned when Take() returns, and the pre-schema backlog can outlive
                // it, so attachments cannot alias the loaned payload itself. A sample that carries
                // any therefore costs ONE owning copy of its body — down from one copy per
                // attachment — and the blobs alias that. §8/§11 assign removing this last copy to
                // the loaned-sample stage by name.
                //
                // A sample with NO attachments — the hot path, and the one the loanable reader
                // exists for — is untouched: no owner, no copy, the row delivered where it lies.
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
                delivery_.OfferView(row, row_len, attachments);
            }
        }
    }

    uint32_t payload_bytes_;

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

// Copying read: deserialize bounds itself by payload.length, so short nodes are safe.
class DataReaderListener : public DataReaderListenerBase {
   public:
    using DataReaderListenerBase::DataReaderListenerBase;

   private:
    void Take(eprosima::fastdds::dds::DataReader* reader) override {
        ReceivedData data;
        eprosima::fastdds::dds::SampleInfo info;
        eprosima::fastdds::dds::ReturnCode_t rc;
        while ((rc = reader->take_next_sample(&data, &info)) ==
               eprosima::fastdds::dds::RETCODE_OK) {
            if (!info.valid_data) continue;
            delivery_.Offer(data.decoded_row, data.decoded_attachments);
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
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_
