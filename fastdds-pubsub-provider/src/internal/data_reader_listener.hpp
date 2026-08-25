// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The two ways DDS lets a DataReader hand samples over, one class each, both feeding
// OrderedDelivery (see internal/ordered_delivery.hpp) which preserves writer order across the
// schema handoff. The schema arrives separately, on the companion __schema channel, via SetSchema.
//
//   LoanableDataReaderListener  take(LoanableSequence<FletcherSample<N>>&, SampleInfoSeq&) — the
//                               reader lends its own payloads, read in place, no copy. Templated on
//                               the provider's payload bound, since that is the struct it reads.
//   DataReaderListener          take_next_sample(&ReceivedData, &SampleInfo) — Fast DDS calls
//                               FletcherSamplePubSubType::deserialize and the row is copied out.
//                               Needs no bound: it reads what arrived.
//
// Both derive from DataReaderListenerBase, which owns everything that is not the read itself: the
// delivery queue, the schema handoff, and the statuses worth logging.
//
// These are not a preference. The loaned flow reads the payload *as a FletcherSample<N>*, which is
// only sound while the payload node is at least as large as that struct; CanLoanSamples() decides
// from the reader's own QoS which flow is admissible. Upstream models the same choice the same way
// — its subscriber branches on `zero_copy_` between exactly these two calls
// (test/dds/communication/SubscriberModule.cpp).
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
#include <utility>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "ordered_delivery.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// Whether a reader with this QoS may be read through loans.
//
// A loan hands back a pointer into a payload node and nothing more — no length, no extent. Reading
// it as a FletcherSample<N> is therefore only sound while every node is a whole one, which is what
// a PREALLOCATED* history memory policy guarantees for a bounded type: `fixed_payload_size_` is the
// type's full size (BaseReader.cpp). Under DYNAMIC_RESERVE or DYNAMIC_REUSABLE the pool
// allocates per arrival at the size that arrived, so a serialised sample — which this type
// deliberately truncates after the bytes in use — leaves a node smaller than the struct, and
// `FletcherSample<N>::length` would then steer reads past the end of it.
//
// Fast DDS does not gate loans on this: `SampleLoanManager` is built from `is_plain` alone
// (DataReaderImpl.cpp), so it would hand out the loan regardless. Nor does the reader's
// data-sharing check, which requires only a bounded, unkeyed type (DataReaderImpl.cpp) —
// so a DYNAMIC reader can still map a writer's segment and get full-size nodes that way. It is only
// the combination of a DYNAMIC policy and a writer that is *not* sharing memory that leaves short
// nodes, and that is reachable: `default_reader_qos.endpoint()` is the caller's to set.
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

    // This runs on a Fast DDS listener thread, so nothing may escape: the subscribe callback is
    // user code and OrderedDelivery rethrows out of it, which would terminate the process.
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

    // Every remaining callback DataReaderListener declares, so that nothing a DataReader can report
    // is left on the default no-op — the subscribe-side counterpart of
    // internal::DataWriterListener. None of them is recoverable here; the point is that they stop
    // being silent, because each has a plausible cause an operator can act on.

    // The mirror of on_publication_matched. Losing the last writer is the half worth hearing about:
    // nothing will arrive and the reader looks idle rather than disconnected. Gaining one is
    // routine, so it goes to INFO, which needs FASTDDS_ENFORCE_LOG_INFO to appear at all
    // (Log.hpp).
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

    // Fletcher sets no DEADLINE, so this only fires on a reader an operator gave one to through
    // FastDDSProviderOptions. Implemented so that a configured policy cannot fail silently.
    void on_requested_deadline_missed(
        eprosima::fastdds::dds::DataReader* reader,
        const eprosima::fastdds::dds::RequestedDeadlineMissedStatus& status) final {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << reader->get_topicdescription()->get_name()
                                           << "' missed its requested deadline, "
                                           << status.total_count << " times in all");
    }

    // Unlike the writer's on_liveliness_lost, this one fires under Fletcher's own QoS:
    // `alive_count` rises when a matched writer first asserts liveliness. With LIVELINESS left at
    // AUTOMATIC and an infinite lease, a writer can only become *not* alive by going away without
    // an orderly unmatch, which is the case worth a warning.
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

    // A QoS mismatch is the worst of them: the endpoints simply never match, so the symptom is a
    // subscriber that stays unconnected forever with nothing logged. FastDDSProviderOptions lets
    // callers set writer and reader QoS independently, which is exactly how that happens.
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

    // Supplies the schema once known. Delivers backlog + live samples in order;
    // runs the callback OUTSIDE any provider lock (it may call back in).
    void SetSchema(SharedSchema schema) { delivery_.SetSchema(std::move(schema)); }

   protected:
    virtual void Take(eprosima::fastdds::dds::DataReader* reader) = 0;

    OrderedDelivery delivery_;
};

// Zero-copy read: the samples stay in the payloads Fast DDS already holds — the writer's
// data-sharing segment when it is on this host — and reach the callback in place.
// FASTDDS_CONST_SEQUENCE is how upstream declares the collection
// (examples/cpp/delivery_mechanisms/PubSubApp.cpp).
template <uint32_t N>
class LoanableDataReaderListener : public DataReaderListenerBase {
   public:
    using DataReaderListenerBase::DataReaderListenerBase;

   private:
    using Sample = FletcherSample<N>;
    FASTDDS_CONST_SEQUENCE(SampleSeq, Sample);

    void Take(eprosima::fastdds::dds::DataReader* reader) override {
        SampleSeq samples;
        eprosima::fastdds::dds::SampleInfoSeq infos;
        // One Attachments for every sample this listener sees. ParseEnvelopeBody clears and
        // refills it, and OrderedDelivery copies it only on the queueing path, so nothing needs
        // a fresh one per sample — and on MSVC a fresh one is not free: an empty unordered_map
        // allocates a sentinel node in its default constructor, measured at 51 ns, paid on every
        // sample of a topic that usually carries no attachments at all.
        Attachments attachments;
        // A loan is only granted while the collection owns nothing (DataReaderImpl.cpp);
        // pre-sizing it would silently switch this to a deserialising take into 1-byte elements.
        assert(samples.maximum() == 0);
        while (reader->take(samples, infos) == eprosima::fastdds::dds::RETCODE_OK) {
            // Returns the loan however the loop exits — ~LoanableSequence does not, it warns and
            // leaks, and a leaked loan permanently costs the reader a payload slot.
            LoanReturn loan_return{reader, samples, infos};
            for (eprosima::fastdds::dds::LoanableCollection::size_type i = 0; i < samples.length();
                 ++i) {
                if (!infos[i].valid_data) continue;
                const Sample& sample = samples[i];
                // A data-sharing writer that laps the reader overwrites the slot being held. Check
                // for that BEFORE reading `length`, since that field steers every read below, and
                // again after, since the body can be overwritten while it is being parsed. Sizing
                // reader history at or above writer history avoids the lap entirely. Only the
                // loaned flow needs this: reading in place is what exposes it.
                if (!reader->is_sample_valid(&sample, &infos[i])) continue;
                if (sample.length > N) continue;
                const uint8_t* row = nullptr;
                uint32_t row_len = 0;

                if (!ParseEnvelopeBody(sample.body, sample.length, row, row_len, attachments))
                    continue;
                if (!reader->is_sample_valid(&sample, &infos[i])) continue;
                delivery_.OfferView(row, row_len, attachments);
            }
        }
    }

    struct LoanReturn {
        eprosima::fastdds::dds::DataReader* reader;
        SampleSeq& samples;
        eprosima::fastdds::dds::SampleInfoSeq& infos;
        ~LoanReturn() {
            // A leaked loan permanently costs the reader a payload slot, and delete_datareader then
            // refuses with RETCODE_PRECONDITION_NOT_MET for the life of the process.
            if (reader->return_loan(samples, infos) != eprosima::fastdds::dds::RETCODE_OK) {
                EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                                   "return_loan failed; a payload slot is lost");
            }
        }
    };
};

// Copying read, for a reader whose payload nodes cannot be assumed whole. Fast DDS deserialises
// into a ReceivedData through FletcherSamplePubSubType::deserialize, which reads the length out of
// the payload and bounds itself by `payload.length` — so it needs nothing from the node beyond the
// bytes that actually arrived. No lap check either: a reader on this path is not sharing memory
// with the writer, so there is no slot underneath it to overwrite.
class DataReaderListener : public DataReaderListenerBase {
   public:
    using DataReaderListenerBase::DataReaderListenerBase;

   private:
    void Take(eprosima::fastdds::dds::DataReader* reader) override {
        ReceivedData data;
        eprosima::fastdds::dds::SampleInfo info;
        while (reader->take_next_sample(&data, &info) == eprosima::fastdds::dds::RETCODE_OK) {
            if (!info.valid_data) continue;
            delivery_.Offer(data.decoded_row, data.decoded_attachments);
        }
    }
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_READER_LISTENER_HPP_
