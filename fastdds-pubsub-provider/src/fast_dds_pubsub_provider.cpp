// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Targets eProsima Fast DDS 3.4.x (fast-dds/3.4.0 from Conan Center). Comments across this
// provider cite upstream by file and symbol against that tree — no line numbers, which a point
// release invalidates without anything noticing.
//
// This file holds the provider itself: the pimpl, the participant/publisher/subscriber lifecycle,
// and the four PubSubProvider methods. On the reader side, data delivery runs through a Fast DDS
// DataReaderListener, and one schema WaitSet thread creates each topic's data reader once its
// schema — and the payload bound announced with it — arrives.
//
// No Arrow C++ dependency anywhere in the path: rows arrive as encoded bytes and leave as encoded
// bytes.

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

#include <cstdint>
#include <cstring>
#include <fastdds/dds/core/Time_t.hpp>
#include <fastdds/dds/core/condition/Condition.hpp>
#include <fastdds/dds/core/condition/GuardCondition.hpp>
#include <fastdds/dds/core/condition/StatusCondition.hpp>
#include <fastdds/dds/core/condition/WaitSet.hpp>
#include <fastdds/dds/core/status/StatusMask.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/SubscriberQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fletcher/core/internal/delivery_frame.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/delivery_channel.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/payload_bound.hpp>
#include <fletcher/pubsub/schema_arrival.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "internal/data_reader_listener.hpp"
#include "internal/data_writer_listener.hpp"
#include "internal/envelope_codec.hpp"
#include "internal/fletcher_sample_pub_sub_type.hpp"
#include "internal/participant_listener.hpp"
#include "internal/profile_document.hpp"
#include "internal/qos_defaults.hpp"
#include "internal/sample_writer.hpp"
#include "internal/transport_data.hpp"

using namespace eprosima::fastdds::dds;

namespace fletcher {

// -----------------------------------------------------------------------
// Impl — hides all Fast DDS types behind the pimpl wall.
// -----------------------------------------------------------------------

struct FastDDSPubSubProvider::Impl {
    // The listeners that forward statuses all need the observer at construction, so it arrives
    // here rather than being assigned afterwards.
    explicit Impl(FastDDSStatusListener* listener)
        : status_listener(listener),
          data_writer_listener(listener),
          participant_listener(listener) {}

    struct TopicState {
        // One DDS Topic per name per participant, so both roles share these two. `data_topic` is
        // under `schema_mu`: the schema thread creates and replaces it. `schema_topic` is under
        // `mu` alone -- no thread but an application thread ever touches that one.
        Topic* data_topic = nullptr;
        Topic* schema_topic = nullptr;

        // The publishing side: two writers, set by CreateTopic. Under `mu`; the schema thread
        // reads `data_writer` and `payload_bytes` under `schema_mu`, which CreateTopic holds while
        // writing both.
        struct Publication {
            DataWriter* data_writer = nullptr;
            DataWriter* schema_writer = nullptr;
            // The bytes the announcement sent: the conflict key for a re-declaration and the
            // schema a subscription on this same instance starts from.
            std::vector<uint8_t> schema_ipc;
            // The bound this topic's writer registered and announces on `__schema` -- this
            // topic's own, from `TopicOptions::max_payload_bytes` or the provider's own bound when
            // that was zero. Set once, when `data_writer` is created, and never changed after: a
            // re-declaration at a different bound is refused rather than migrating the writer.
            uint32_t payload_bytes = 0;
            // The `<data_writer>` profile this topic's writer was resolved from
            // (`TopicOptions::profile`), empty when none was given. The conflict key for a
            // re-declaration alongside `payload_bytes`, above.
            std::string profile;
        } published;

        // The subscribing side: two readers. Every field the schema thread touches is written
        // under `schema_mu`; `watches` is the exception -- only an application thread, under `mu`,
        // ever touches that one. Non-empty (`schema_reader || schema`) means the schema channel is
        // open; `data_listener` set means a data subscription is live, its reader open
        // (`data_reader`) or still waiting for the schema; `watches` counts SubscribeSchema
        // holders sharing the channel.
        struct Subscription {
            DataReader* schema_reader = nullptr;
            SchemaArrival arrival;
            // Dropping an unconsumed resolver IS the "subscription ended" outcome, so a teardown
            // moves it into a local that dies; every Resolve/Fail checks valid() first.
            SchemaResolver resolver;
            std::vector<uint8_t> schema_ipc;
            SharedSchema schema;
            uint32_t payload_bytes = 0;
            uint32_t watches = 0;
            DataReader* data_reader = nullptr;
            std::unique_ptr<internal::DataReaderListenerBase> data_listener;
            // The `<data_reader>` profile this subscription was opened with
            // (`TopicOptions::profile`), empty when none was given. Written under `schema_mu`
            // alongside `data_listener` (Subscribe) and cleared there too (Unsubscribe);
            // `OpenDataReader` reads it on the schema thread, under the same lock.
            std::string profile;
        } subscribed;
    };

    // The map entry a TopicState lives in: `topics` is never erased (comment below), so a pointer
    // to one of its entries is stable for the provider's life -- what lets the schema registry key
    // off the entry directly instead of a name that has to be looked up again.
    using TopicEntry = std::pair<const std::string, TopicState>;

    // Non-owning and possibly null: the application's observer for endpoint and discovery status
    // (public header). Handed to every listener this provider creates; null means nothing is
    // observed, which is what construction from a `ProviderConfig` alone gives.
    FastDDSStatusListener* status_listener = nullptr;

    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;

    // Shared for Publish, exclusive for everything else; guards `topics`, `published` and
    // `schema_topic`.
    // DataWriter::write is itself thread safe, so a shared lock is enough to keep the topic and its
    // writer alive for the duration of the call, and publishes to different topics then run
    // concurrently instead of serialising on this mutex. See README "Measured decisions".
    std::shared_mutex mu;
    // Unordered because Publish looks a topic up by name on every sample and a hash beats the
    // std::map this was: log-n string comparisons per publish bought nothing. Never erased, which
    // is what lets the schema thread hold a bare `TopicEntry*` in `schema_registry` for as long as
    // that entry is registered without it dangling.
    std::unordered_map<std::string, TopicState> topics;

    // The registered type is built from this one number.
    uint32_t max_payload_bytes = 0;

    // Shared by every DataWriter this provider creates; carries no per-topic state either.
    internal::DataWriterListener data_writer_listener;

    // Installed on the participant with StatusMask::none() (internal/participant_listener.hpp),
    // unconditionally: the discovery callbacks are not mask-gated, and with a null observer each
    // one is a single branch at discovery rate. `~Impl` deletes the participant in its body, so
    // this member is still alive when the last callback returns.
    internal::ParticipantListener participant_listener;

    // This provider's one schema thread: owns `schema_wait_set` and serves every subscribed
    // topic's __schema reader off it (SchemaLoop, below) for the provider's whole life -- started
    // in the constructor once `subscriber` exists (`schema_thread_stop` attached first), stopped
    // and joined FIRST in `~Impl`, before any entity it might still be draining is deleted.
    // `schema_registry` maps a schema reader's StatusCondition to the TopicEntry it belongs to;
    // EnsureSchemaChannel inserts an entry (under `schema_mu`) before attaching that condition to
    // `schema_wait_set`, so a wake that fires the instant the condition attaches can never find the
    // entry missing. Unsubscribe/UnsubscribeSchema erase it the same way, before detaching.
    //
    // `schema_mu` guards `subscribed`, `data_topic`, the schema registry, and the reads of
    // `published.data_writer` the schema thread makes. Lock order: `mu` -> `schema_mu`; the schema
    // thread never takes `mu`. `CloseSchemaSide` takes `schema_mu` itself, so it is never called
    // with it held. Both `mu` and `schema_mu` are held across a Fast DDS call at points below
    // (`create_topic`, `find_type`/`register_type`, `create_datareader`, `create_datawriter`,
    // `delete_datawriter`, `delete_topic`, `write`, `attach_condition`) -- safe because none of
    // those calls synchronously calls back into this provider's own code except the status
    // listener's `OnMatched`, which is forbidden from re-entering the provider (public header) --
    // so there is no cycle back into either mutex. `delete_datareader` and `detach_condition` run
    // under neither lock, deliberately: deleting a reader waits for any in-flight callback on it.
    // Data readers are otherwise not this thread's concern: their statuses and samples are
    // delivered by their own DataReaderListenerBase (internal/data_reader_listener.hpp), on
    // whichever thread Fast DDS invokes it on.
    eprosima::fastdds::dds::WaitSet schema_wait_set;
    eprosima::fastdds::dds::GuardCondition schema_thread_stop;
    std::mutex schema_mu;
    std::unordered_map<const eprosima::fastdds::dds::Condition*, TopicEntry*> schema_registry;
    std::thread schema_thread;

    // Reads and forwards every status `reader` (a SCHEMA DataReader only -- a data reader's
    // statuses are forwarded by its own DataReaderListenerBase, internal/data_reader_listener.hpp)
    // might have changed, via status_endpoint.hpp's ReaderEndpoint, in the fixed order below. The
    // return value is what `SchemaLoop` drains on (`data_available`). Every `get_*_status` call
    // below is read-and-clear per DDS, so each runs at most once per call, REGARDLESS of whether
    // `status_listener` is set: skipping a changed status's getter when there is nobody to forward
    // it to would leave it "changed" forever.
    bool DispatchReaderStatuses(DataReader* reader) {
        const StatusMask changed = reader->get_status_changes();
        if (changed.is_active(StatusMask::subscription_matched())) {
            SubscriptionMatchedStatus s;
            reader->get_subscription_matched_status(s);
            if (status_listener)
                status_listener->OnMatched(internal::ReaderEndpoint(reader), s.current_count,
                                           s.current_count_change);
        }
        if (changed.is_active(StatusMask::requested_deadline_missed())) {
            RequestedDeadlineMissedStatus s;
            reader->get_requested_deadline_missed_status(s);
            if (status_listener)
                status_listener->OnDeadlineMissed(internal::ReaderEndpoint(reader), s.total_count);
        }
        if (changed.is_active(StatusMask::liveliness_changed())) {
            LivelinessChangedStatus s;
            reader->get_liveliness_changed_status(s);
            if (status_listener)
                status_listener->OnLivelinessChanged(internal::ReaderEndpoint(reader),
                                                     s.alive_count, s.not_alive_count);
        }
        if (changed.is_active(StatusMask::requested_incompatible_qos())) {
            RequestedIncompatibleQosStatus s;
            reader->get_requested_incompatible_qos_status(s);
            if (status_listener)
                status_listener->OnIncompatibleQos(internal::ReaderEndpoint(reader),
                                                   static_cast<uint32_t>(s.last_policy_id),
                                                   s.total_count);
        }
        if (changed.is_active(StatusMask::sample_lost())) {
            SampleLostStatus s;
            reader->get_sample_lost_status(s);
            if (status_listener)
                status_listener->OnSampleLost(internal::ReaderEndpoint(reader),
                                              static_cast<uint32_t>(s.total_count));
        }
        if (changed.is_active(StatusMask::sample_rejected())) {
            SampleRejectedStatus s;
            reader->get_sample_rejected_status(s);
            if (status_listener)
                status_listener->OnSampleRejected(internal::ReaderEndpoint(reader),
                                                  static_cast<int32_t>(s.last_reason),
                                                  s.total_count);
        }
        return changed.is_active(StatusMask::data_available());
    }

    // This provider's one schema thread: every subscribed topic's __schema reader, off one
    // WaitSet. Holds `schema_mu` for the whole pass, including the settle HandleSchema runs
    // through the resolver -- which is what keeps a Subscribe/Unsubscribe on this same topic
    // (they also take `schema_mu`, see its comment above) from observing a half-updated
    // TopicState.
    void SchemaLoop() {
        for (;;) {
            ConditionSeq active;
            // Waits without a timeout: a wake is a status change on a registered schema reader, or
            // the stop guard.
            const ReturnCode_t wait_rc = schema_wait_set.wait(active, c_TimeInfinite);
            if (schema_thread_stop.get_trigger_value()) return;

            // Tolerant wait (ddsbus WaitsetDataReader::listen): a non-OK return means `active`
            // cannot be trusted, not that this thread should stop -- go back and wait again.
            if (wait_rc != RETCODE_OK) continue;
            std::lock_guard lock(schema_mu);
            for (Condition* c : active) {
                // Lookup by pointer value only -- never dereferenced before this. A miss is a
                // topic torn down between the wake and this loop (Unsubscribe/UnsubscribeSchema
                // erase the entry before detaching, see their comments), or `schema_thread_stop`'s
                // own condition alongside a real one in the same wake -- either way, nothing to do.
                auto it = schema_registry.find(c);
                if (it == schema_registry.end()) continue;
                TopicEntry& entry = *it->second;
                // A throw stays scoped to this one topic, so a schema-side failure on one topic
                // never skips another's wake in the same pass.
                try {
                    if (DispatchReaderStatuses(entry.second.subscribed.schema_reader))
                        HandleSchema(entry.first, entry.second);
                } catch (const std::exception& ex) {
                    EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                       "reading a schema sample threw: " << ex.what());
                } catch (...) {
                    EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                       "reading a schema sample threw a non-std exception");
                }
            }
        }
    }

    // The registered type name for `payload_bytes`, registering the type on first use. The
    // participant owns every registered TypeSupport (DomainParticipant::find_type returns it), so
    // nothing is kept here. Precondition: caller holds `schema_mu` -- two threads registering the
    // same name at once would race to a PRECONDITION_NOT_MET.
    std::string DataTypeNameFor(uint32_t payload_bytes) {
        std::string type_name = FletcherTypeName(payload_bytes);
        if (participant->find_type(type_name).empty()) {
            TypeSupport type_support(new internal::FletcherSamplePubSubType(payload_bytes));
            if (type_support.register_type(participant) != RETCODE_OK)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to register the data type for payload bound " +
                                      std::to_string(payload_bytes));
        }
        return type_name;
    }

    // Creates `ts`'s data reader, enabled, with its listener installed, for the bound announced
    // with its schema. Precondition: caller holds `schema_mu`; `ts.subscribed.schema`,
    // `ts.subscribed.payload_bytes` and `ts.subscribed.data_listener` are set;
    // `ts.subscribed.data_reader` is null. Throws `PubSubError(kTransportFailure)` with
    // `ts.subscribed.data_reader` still null and no Fast DDS entity leaked.
    void OpenDataReader(const std::string& name, TopicState& ts) {
        auto& sub = ts.subscribed;
        const uint32_t bound = sub.payload_bytes;
        const std::string want = FletcherTypeName(bound);
        if (ts.data_topic && ts.data_topic->get_type_name() != want) {
            if (ts.published.data_writer) {
                // This provider publishes the topic at its own bound, and a DDS topic has one type
                // per participant: its own bound wins, and the remote publisher that announced
                // another one will not match this reader.
                EPROSIMA_LOG_ERROR(
                    FLETCHER_SUBSCRIPTION,
                    "'" << name << "' is published by this instance at payload bound "
                        << ts.published.payload_bytes << "; a remote publisher announced " << bound
                        << " and will not match this reader");
            } else {
                // Left behind by an earlier subscription that followed a publisher on another
                // bound (the publisher restarted with a new one). Its reader is gone, so the
                // topic can be replaced by one of the announced bound's type.
                if (participant->delete_topic(ts.data_topic) != RETCODE_OK)
                    throw PubSubError(PubSubStatus::kTransportFailure,
                                      "FastDDS: failed to replace the data topic of '" + name +
                                          "' for payload bound " + std::to_string(bound));
                ts.data_topic = nullptr;
            }
        }
        if (!ts.data_topic) {
            ts.data_topic =
                participant->create_topic(name, DataTypeNameFor(bound), TOPIC_QOS_DEFAULT);
            if (!ts.data_topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create topic: " + name);
        }
        const DataReaderQos rqos = internal::ResolveDataReaderQos(*subscriber, name, sub.profile);
        // SetSchema before the reader can deliver: Drain asserts it.
        sub.data_listener->SetSchema(sub.schema);
        DataReader* data_reader = subscriber->create_datareader(
            ts.data_topic, rqos, sub.data_listener.get(), StatusMask::all());
        if (!data_reader)
            throw PubSubError(PubSubStatus::kTransportFailure,
                              "FastDDS: failed to create DataReader for: " + name);
        sub.data_reader = data_reader;
    }

    // Drains `ts.subscribed.schema_reader`. Called only from the schema thread, with `schema_mu`
    // held -- which is also what makes reading `ts.subscribed.data_reader` here safe: every
    // application-thread write to it is made under the same lock (the comment on `schema_mu`
    // above).
    //
    // The first decodable sample carrying a usable bound settles `ts.subscribed.resolver` and --
    // if a data reader is already waiting on this topic and not yet open -- opens it:
    // OpenDataReader, the same thing Subscribe does when the schema is already known when it runs,
    // just on this thread instead. Every later sample is only compared against the first's bytes
    // (kept in `ts.subscribed.schema_ipc`): a resend of the same schema is silent, a genuine
    // conflict is a diagnostic only -- CreateTopic already owns refusal for a conflict raised
    // locally. Schema history is one sample deep (MakeSchemaChannelReaderQos), so there is at most
    // one to take per call; no per-sample stop check is needed the way a longer drain would want.
    void HandleSchema(const std::string& name, TopicState& ts) {
        auto& sub = ts.subscribed;
        internal::ReceivedData sample;
        SampleInfo info;
        ReturnCode_t rc;
        while ((rc = sub.schema_reader->take_next_sample(&sample, &info)) == RETCODE_OK) {
            if (!info.valid_data) continue;

            // Decoded once per sample: the resend/conflict branch below compares it against the
            // bound this reader already follows, the first-arrival path validates it with
            // IsPayloadBound.
            const Blob* bound_blob = sample.decoded_attachments.Find(kSchemaPayloadBoundKey);
            uint32_t bound = 0;
            const bool bound_present = bound_blob && bound_blob->size() == sizeof(bound);
            if (bound_present) std::memcpy(&bound, bound_blob->data(), sizeof(bound));

            if (sub.schema) {
                // A resend of the same schema (fan-in) is silent; a genuine conflict is
                // diagnostic-only here -- CreateTopic already owns refusal for a local one.
                if (sample.decoded_row != sub.schema_ipc) {
                    EPROSIMA_LOG_ERROR(
                        FLETCHER_SCHEMA,
                        "a second publisher announced a different schema for this topic; the "
                        "first announcement stands and this one is ignored");
                }
                // A second publisher's bound is compared too, tolerating one that carries none:
                // it will never match this reader, opened for the first announcement's bound.
                if (bound_present && bound != sub.payload_bytes) {
                    EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                       "a second publisher announced payload bound "
                                           << bound
                                           << " for this topic; this reader follows the "
                                              "first announcement's "
                                           << sub.payload_bytes
                                           << " and that publisher will not match");
                }
                continue;
            }

            // Deserialize before resolving: a malformed sample must not throw out of the reader
            // thread, nor leave this topic's schema "known" with nothing to show for it.
            OwnedSchema owned;
            try {
                owned = DeserializeSchemaIpc(sample.decoded_row.data(), sample.decoded_row.size());
            } catch (const std::exception& e) {
                EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                   "ignoring a schema sample that will not decode ("
                                       << sample.decoded_row.size() << " bytes): " << e.what());
                if (sub.resolver.valid())
                    std::move(sub.resolver)
                        .Fail(PubSubStatus::kInternal,
                              std::string("FastDDS: schema sample would not decode: ") + e.what());
                return;
            } catch (...) {
                EPROSIMA_LOG_ERROR(
                    FLETCHER_SCHEMA,
                    "ignoring a schema sample that will not decode: non-std exception");
                if (sub.resolver.valid())
                    std::move(sub.resolver)
                        .Fail(PubSubStatus::kInternal,
                              "FastDDS: schema sample would not decode: non-std exception");
                return;
            }

            if (!IsPayloadBound(bound)) {
                EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                   "ignoring a schema sample that carries no usable "
                                       << kSchemaPayloadBoundKey << " attachment (" << bound
                                       << ")");
                if (sub.resolver.valid())
                    std::move(sub.resolver)
                        .Fail(PubSubStatus::kInternal,
                              "FastDDS: schema sample carries no usable max_payload_bytes "
                              "attachment");
                return;
            }

            sub.schema_ipc = sample.decoded_row;
            sub.schema = MakeSharedSchema(std::move(owned));
            sub.payload_bytes = bound;

            // Create BEFORE resolving. A waiter woken by the arrival may publish at once, and with
            // VOLATILE data a row sent before this reader exists and is matched is gone; Fast DDS
            // completes intraprocess matching inside create_datareader, so a resolved arrival
            // means the reader is live for a same-process publisher.
            if (sub.data_listener && !sub.data_reader) {
                try {
                    OpenDataReader(name, ts);
                } catch (const std::exception& e) {
                    // Broad on purpose: the announced bound sizes the reader's pool, so a huge one
                    // can fail allocation inside create_datareader.
                    EPROSIMA_LOG_ERROR(
                        FLETCHER_SUBSCRIPTION,
                        "data reader could not be opened after its schema arrived: " << e.what());
                    if (sub.resolver.valid())
                        std::move(sub.resolver)
                            .Fail(PubSubStatus::kTransportFailure,
                                  std::string("FastDDS: the data reader could not be opened once "
                                              "its schema arrived: ") +
                                      e.what());
                    continue;
                } catch (...) {
                    EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                                       "data reader could not be opened after its schema arrived: "
                                       "non-std exception");
                    if (sub.resolver.valid())
                        std::move(sub.resolver)
                            .Fail(PubSubStatus::kTransportFailure,
                                  "FastDDS: the data reader could not be opened once its schema "
                                  "arrived: non-std exception");
                    continue;
                }
            }
            // Settles SchemaArrival's own state and wakes any Wait() caller; it runs no user code
            // (SchemaResolver::Resolve, schema_arrival.cpp), so a throw from this line is caught by
            // SchemaLoop's per-topic try/catch like everything else in this function.
            if (sub.resolver.valid()) std::move(sub.resolver).Resolve(sub.schema);
        }
        if (rc != RETCODE_NO_DATA) {
            EPROSIMA_LOG_WARNING(FLETCHER_SCHEMA,
                                 "schema reader take_next_sample failed with return code "
                                     << rc << "; the rest of this notification was not read");
        }
    }

    // Opens the topic's schema channel if it has none. Caller holds `mu` exclusive.
    //
    // Resolved on the spot when this provider published the topic -- no reader needed. Otherwise
    // fed by a __schema reader this function creates enabled, registered with the schema thread
    // (under `schema_mu`, see the comment at the registration below) before it is attached to the
    // waitset: registered first, so a wake -- which needs the condition attached -- always finds
    // the entry; a condition already triggered when it is attached wakes the thread at once, so a
    // sample that lands between creation and attach is not missed either.
    void EnsureSchemaChannel(TopicEntry& entry) {
        const std::string& name = entry.first;
        TopicState& ts = entry.second;

        // Open already: the publisher branch below leaves `subscribed.schema` set, the subscriber
        // branch leaves `subscribed.schema_reader` set, and every teardown nulls both together.
        // Read under `mu` alone, which is enough for both: only an application thread writes
        // `schema_reader`, under this same lock, and while it reads null this topic has no registry
        // entry -- so the schema thread cannot be writing `schema` underneath the second test.
        if (ts.subscribed.schema_reader || ts.subscribed.schema) return;

        if (!ts.published.schema_ipc.empty()) {
            // Publisher-side: nothing to wait for, and no reader to create. Starts from the bytes
            // the announcement sent, not from a kept OwnedSchema (CreateTopic drops it once it is
            // serialized). Decoded before anything in the topic state is written, so a decode that
            // throws leaves the channel closed rather than holding an arrival nothing can settle.
            SharedSchema schema = MakeSharedSchema(DeserializeSchemaIpc(
                ts.published.schema_ipc.data(), ts.published.schema_ipc.size()));
            std::lock_guard lock(schema_mu);
            std::tie(ts.subscribed.arrival, ts.subscribed.resolver) = SchemaArrival::Create();
            ts.subscribed.schema = std::move(schema);
            // The bound this topic announced, which is this TOPIC's own and not necessarily the
            // provider's: a subscription on this same instance follows the announcement exactly as
            // a remote one does, or OpenDataReader would look for a type the data topic does not
            // carry. Non-empty `schema_ipc` means CreateTopic ran, so `payload_bytes` is set.
            ts.subscribed.payload_bytes = ts.published.payload_bytes;
            std::move(ts.subscribed.resolver).Resolve(ts.subscribed.schema);
            return;
        }

        std::string schema_topic_name = name + "/__schema";
        if (!ts.schema_topic) {
            ts.schema_topic =
                participant->create_topic(schema_topic_name, kSchemaTypeName, TOPIC_QOS_DEFAULT);
            if (!ts.schema_topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create schema topic: " + schema_topic_name);
        }

        // `listener = nullptr, StatusMask::all()` (ddsbus's WaitsetDataReader pattern): the schema
        // thread reads statuses off the StatusCondition itself (DispatchReaderStatuses above), so
        // nothing needs a push callback here. The condition is left at its default enabled mask --
        // not narrowed to data_available -- so it triggers for every status, not only new samples.
        DataReader* schema_reader = subscriber->create_datareader(
            ts.schema_topic, internal::MakeSchemaChannelReaderQos(), nullptr, StatusMask::all());
        if (!schema_reader)
            throw PubSubError(
                PubSubStatus::kTransportFailure,
                "FastDDS: failed to create schema DataReader for: " + schema_topic_name);

        // `schema_registry[cond] = &entry` and `ts.subscribed.schema_reader = schema_reader` must
        // become visible to the schema thread together: SchemaLoop dereferences
        // `entry.second.subscribed.schema_reader` right after a registry hit, under this same
        // lock.
        {
            std::lock_guard lock(schema_mu);
            ts.subscribed.schema_reader = schema_reader;
            std::tie(ts.subscribed.arrival, ts.subscribed.resolver) = SchemaArrival::Create();
            schema_registry[&schema_reader->get_statuscondition()] = &entry;
        }

        schema_wait_set.attach_condition(schema_reader->get_statuscondition());
    }

    // Closes the topic's schema side. Caller holds `mu` exclusive and has decided the side must go
    // (no watch left, no data reader left). Everything the schema thread reads is nulled under
    // `schema_mu`; the returned reader — null when this provider published the topic itself and
    // never opened one — is detached and deleted by the caller after `mu` is released, so other
    // topics stay publishable while a slow delete_datareader runs. `subscribed.schema` goes with
    // the reader: a later channel on this topic must start from "not yet known", or HandleSchema
    // would treat a fresh reader's first sample as an already-resolved resend
    // (ResubscribeAfterUnsubscribeKeepsDelivering hung on exactly that). The resolver dies here
    // under both `mu` and `schema_mu`; that is safe because settling runs no user code
    // (SchemaArrivalState::Settle only locks its own mutex and notifies).
    DataReader* CloseSchemaSide(TopicState& ts) {
        std::lock_guard lock(schema_mu);
        auto& sub = ts.subscribed;
        DataReader* schema_reader = sub.schema_reader;
        if (schema_reader) schema_registry.erase(&schema_reader->get_statuscondition());
        sub.schema_reader = nullptr;
        sub.schema = nullptr;
        sub.payload_bytes = 0;
        // Moved into a local that dies at the end of this function: ~SchemaResolver is what settles
        // any pending waiter with kSubscriptionEnded (assigning over the field would skip it).
        SchemaResolver ended = std::move(sub.resolver);
        return schema_reader;
    }

    // Teardown lives here, not in ~FastDDSPubSubProvider, so a constructor that throws part-way
    // still releases whatever it had already created — a throwing constructor means the outer
    // destructor never runs, but impl_ still unwinds.
    ~Impl() {
        if (!participant) return;

        // The schema thread stopped and joined FIRST, before any entity it might still be
        // draining is deleted below -- `schema_wait_set` and `schema_registry` die with this
        // object right after, which is why nothing here bothers detaching a condition from either.
        schema_thread_stop.set_trigger_value(true);
        if (schema_thread.joinable()) schema_thread.join();

        for (auto& [name, ts] : topics) {
            // Delete the schema reader first: it stops feeding the schema channel before the rest
            // is torn down.
            if (ts.subscribed.schema_reader)
                subscriber->delete_datareader(ts.subscribed.schema_reader);
            if (ts.published.schema_writer)
                publisher->delete_datawriter(ts.published.schema_writer);
            if (ts.published.data_writer) publisher->delete_datawriter(ts.published.data_writer);
            if (ts.subscribed.data_reader) subscriber->delete_datareader(ts.subscribed.data_reader);
            if (ts.schema_topic) participant->delete_topic(ts.schema_topic);
            if (ts.data_topic) participant->delete_topic(ts.data_topic);
        }
        topics.clear();

        if (publisher) participant->delete_publisher(publisher);
        if (subscriber) participant->delete_subscriber(subscriber);

        DomainParticipantFactory::get_instance()->delete_participant(participant);
    }
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

// A registry factory is handed a `ProviderConfig` and nothing else, so a provider reached through
// one observes no statuses. Passing a listener means constructing the provider directly.
void RegisterFastDDSProvider(ProviderRegistry& registry) {
    registry.Register("fastdds", [](const ProviderConfig& config) {
        return std::make_shared<FastDDSPubSubProvider>(config);
    });
}

FastDDSPubSubProvider::FastDDSPubSubProvider(const ProviderConfig& config)
    : FastDDSPubSubProvider(config, nullptr) {}

FastDDSPubSubProvider::FastDDSPubSubProvider(const ProviderConfig& config,
                                             FastDDSStatusListener* status_listener)
    : impl_(std::make_unique<Impl>(status_listener)) {
    // Everything the document decides about the PARTICIPANT — the domain — is settled below,
    // before the participant exists. An empty `config.document` is resolved to
    // Fletcher's baked-in one first (qos_defaults.cpp), so there is exactly one path.
    // Writer/reader QoS is resolved per topic, in `CreateTopic`, the first moment a topic's name is
    // known (internal/profile_document.hpp). Refusals here are `kInvalidArgument`: reached through
    // a factory, a `std::invalid_argument` would arrive at the caller as `kInternal`, which tells
    // an operator nothing.
    // 64 KiB when unset. The bound governs this provider's publishers: it is the number in the
    // type name CreateTopic registers and the size a published row has to fit. Subscribers follow
    // whatever a publisher announces.
    const uint32_t bound = config.max_payload_bytes == 0 ? 64 * 1024 : config.max_payload_bytes;
    if (!IsPayloadBound(bound)) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "FastDDS: max_payload_bytes " + std::to_string(bound) +
                              " cannot bound a payload; it must be a multiple of 4 between " +
                              std::to_string(kMinPayloadBytes) + " and " +
                              std::to_string(kMaxPayloadBytes));
    }
    impl_->max_payload_bytes = bound;

    // An empty document resolves to Fletcher's baked-in one, so `document` always ends up loaded
    // into Fast DDS's process-wide profile registry (internal::LoadDocumentOnce, called from
    // ResolveParticipantQos below).
    const std::string document = config.document.empty()
                                     ? std::string(internal::FletcherDefaultProfilesDocument())
                                     : config.document;

    DomainParticipantQos pqos;
    internal::ResolveParticipantQos(document, config.domain_id, pqos);

    // Fast DDS's own get_default_datawriter_qos_from_xml / get_default_datareader_qos_from_xml
    // (Publisher.hpp / Subscriber.hpp) would answer "does this document have a default profile"
    // directly, but both re-parse the WHOLE document from scratch on every call
    // (XMLProfileManager::fill_attributes_from_xml -> XMLParser::loadXML -> parseXML), which
    // replays a <log> element's Log::ClearConsumers()/RegisterConsumer() side effect on every
    // construction and logs its own unconditional EPROSIMA_LOG_ERROR(XMLPARSER, "... profile not
    // found") on a miss -- a second, uncontrolled error line this warning must not manufacture. So
    // this looks at the document text instead: does any <data_writer>/<data_reader> START TAG carry
    // the attribute is_default_profile="true", in either quote (XML admits both, and so does the
    // tinyxml2 parser Fast DDS reads the document with)? Text is not a parser, and the residue is
    // published rather than papered over: a spelling with spaces around the `=`, or a
    // <data_writer> inside an XML comment, is read wrong in one direction or the other. This
    // decides a warning, not the QoS.
    const auto has_default_profile = [&document](const char* element) {
        for (auto pos = document.find(element); pos != std::string::npos;
             pos = document.find(element, pos + 1)) {
            const auto tag_end = document.find('>', pos);
            if (tag_end == std::string::npos) return false;
            if (document.find("is_default_profile=\"true\"", pos) < tag_end ||
                document.find("is_default_profile='true'", pos) < tag_end)
                return true;
        }
        return false;
    };
    if (!has_default_profile("<data_writer") || !has_default_profile("<data_reader")) {
        EPROSIMA_LOG_WARNING(FLETCHER_PUBLICATION,
                             "the profile document defines no is_default_profile data_writer/"
                             "data_reader: every topic without a named profile runs on Fast "
                             "DDS's own defaults - a writer history of KEEP_LAST depth 1, so a "
                             "reader one sample behind loses data, and a BEST_EFFORT reader, "
                             "which asks for no retransmission at all; start the document from "
                             "FastDDSPubSubProvider::DefaultProfilesDocument()");
    }

    // StatusMask::none() is load-bearing, not tidiness: a participant listener that holds the
    // `data_on_readers` bit is handed every reader's data INSTEAD of the reader's own listener, so
    // the default all() here would silently take over the data path
    // (internal/participant_listener.hpp has the source citations). Discovery is dispatched
    // regardless of the mask, which is why none() still delivers it.
    impl_->participant = DomainParticipantFactory::get_instance()->create_participant(
        config.domain_id, pqos, &impl_->participant_listener, StatusMask::none());
    if (!impl_->participant)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to create DomainParticipant");

    TypeSupport schema_type(new internal::SchemaBytesPubSubType(kSchemaPayloadBytes));
    if (schema_type.register_type(impl_->participant) != RETCODE_OK)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to register the schema type");

    impl_->publisher = impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!impl_->publisher)
        throw PubSubError(PubSubStatus::kTransportFailure, "FastDDS: failed to create Publisher");

    impl_->subscriber = impl_->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!impl_->subscriber)
        throw PubSubError(PubSubStatus::kTransportFailure, "FastDDS: failed to create Subscriber");

    // The Publisher's/Subscriber's default QoS is already what Fast DDS seeded it with from
    // `document`'s `is_default_profile="true"` profile, if any (else Fast DDS's own default) --
    // done inside create_publisher/create_subscriber above, before this provider touches either.
    // `document` is never empty by the time it is loaded (see above), so this is the only path.

    // This provider's one schema thread, started once the Subscriber it depends on exists:
    // `schema_thread_stop` attached first, so `schema_wait_set` always has something to wake it
    // for, even before any topic's schema reader does. See Impl::SchemaLoop / Impl::HandleSchema.
    impl_->schema_wait_set.attach_condition(impl_->schema_thread_stop);
    impl_->schema_thread = std::thread([impl = impl_.get()] { impl->SchemaLoop(); });
}

// Destruction precondition: no thread may be executing or about to
// enter a public provider API on this instance, and no provider callback may
// still be in flight if that callback can re-enter this provider. Teardown
// invalidates all TopicState pointers; it is NOT a synchronization boundary for
// concurrent use of the provider object.
//
// Teardown deliberately does NOT take impl_->mu.
// DDS deletion calls such as delete_datareader() may block waiting for in-flight
// listener callbacks to finish. If teardown held impl_->mu while such a deletion
// waits, and a callback that re-enters the provider tried to take impl_->mu, the
// two would deadlock (hold-and-wait). This mirrors Unsubscribe(), which extracts
// state under the lock and deletes readers OUTSIDE it. A lock cannot cure
// use-*during*-destruction UB; the quiescence precondition above is the real
// contract. ~Impl (above) honours this — it takes no lock.
//
// Teardown lives in ~Impl so a throwing constructor still releases what it built.
FastDDSPubSubProvider::~FastDDSPubSubProvider() = default;

uint32_t FastDDSPubSubProvider::PayloadBytes() const noexcept { return impl_->max_payload_bytes; }

const char* FastDDSPubSubProvider::DefaultProfilesDocument() noexcept {
    return internal::FletcherDefaultProfilesDocument();
}

// -----------------------------------------------------------------------
// PubSubProvider interface
// -----------------------------------------------------------------------

void FastDDSPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                        OwnedSchema schema) {
    CreateTopicWithOptions(topic_segments, std::move(schema), TopicOptions{});
}

void FastDDSPubSubProvider::CreateTopicWithOptions(const std::vector<std::string>& topic_segments,
                                                   OwnedSchema schema,
                                                   const TopicOptions& options) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number.
    TranslateSeamFailure([&] {
        // The door, BEFORE any lock: a Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        //
        // "Before any lock" is not decoration: with this check one line lower,
        // after `impl_->mu`, the refusal never runs and the call hangs on the
        // mutex exactly as it did with no door at all. That is a measured
        // mistake, not a hypothetical one.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "CreateTopicWithOptions");

        std::string name = internal::JoinSegments(topic_segments);

        // This topic's own bound: `options.max_payload_bytes` if given, else the provider's own
        // (0 there means "unset" too, resolved at construction). Validated before any lock, the
        // same rule the constructor applies to the provider's own bound.
        const uint32_t bound =
            options.max_payload_bytes ? options.max_payload_bytes : impl_->max_payload_bytes;
        if (!IsPayloadBound(bound)) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "FastDDS: max_payload_bytes " + std::to_string(bound) +
                                  " cannot bound a payload; it must be a multiple of 4 between " +
                                  std::to_string(kMinPayloadBytes) + " and " +
                                  std::to_string(kMaxPayloadBytes));
        }

        std::lock_guard lock(impl_->mu);

        // Idempotent, mirroring the in-process reference provider
        // (InProcessPubSubProvider::CreateTopic): declaring a topic never fails on an
        // existing one. The topic state may already exist because a subscriber
        // joined first (subscriber-first) and lazily created it without a schema,
        // or because a publisher already declared it. Attach the publisher side
        // and announce the schema exactly once.
        auto& entry = *impl_->topics.try_emplace(name).first;
        auto& ts = entry.second;

        // The data topic may already exist (created by a prior Subscribe, at whatever bound its
        // reader follows): reuse it if it carries this topic's own bound, replace it if a
        // finished subscription left it at another, refuse while a live subscription holds it at
        // another.
        {
            std::lock_guard schema_lock(impl_->schema_mu);

            if (ts.published.data_writer) {
                // Already declared BY THIS INSTANCE: the data topic is already at this topic's own
                // type and nothing about it needs revisiting. Idempotent for the same (or no) bound
                // and the same (or no) profile, refused for a genuine conflict -- neither field can
                // migrate the writer once created, a caller that wants another bound or profile
                // unsubscribes and declares a fresh topic instead. Only a field the caller actually
                // named conflicts: an empty `TopicOptions` means "the provider's defaults" and is
                // never refused, so it re-declares a topic declared at any bound. This check must
                // run BEFORE the topic-type handling below ever looks at `bound`: that logic exists
                // for the topic this instance does NOT yet publish, and running it here with a
                // conflicting `bound` would delete the very topic this writer is attached to.
                if ((options.max_payload_bytes != 0 &&
                     options.max_payload_bytes != ts.published.payload_bytes) ||
                    (!options.profile.empty() && options.profile != ts.published.profile)) {
                    throw PubSubError(PubSubStatus::kInvalidArgument,
                                      "FastDDS: '" + name +
                                          "' is already declared at payload bound " +
                                          std::to_string(ts.published.payload_bytes) +
                                          " with profile '" + ts.published.profile + "'");
                }
            } else {
                // The data topic may already exist (created by a prior Subscribe, at whatever
                // bound its reader follows): reuse it if it carries this topic's own bound, replace
                // it if a finished subscription left it at another, refuse while a live
                // subscription holds it at another.
                const std::string own_type = FletcherTypeName(bound);
                if (ts.data_topic && ts.data_topic->get_type_name() != own_type) {
                    if (ts.subscribed.data_listener) {
                        // A DDS topic has one type per participant, and a live subscription on
                        // this instance holds this one at the bound its publisher announced. The
                        // bound is quoted off the topic rather than off `subscribed.payload_bytes`,
                        // which an Unsubscribe zeroes while leaving the topic itself behind.
                        throw PubSubError(
                            PubSubStatus::kInvalidArgument,
                            "FastDDS: '" + name + "' already carries type " +
                                ts.data_topic->get_type_name() +
                                " on this instance, from a subscription that followed a "
                                "publisher's announced bound; this instance publishes at " +
                                std::to_string(bound));
                    }
                    // Left behind by a subscription that followed a publisher on another bound and
                    // has since been unsubscribed: nothing references it, so it is replaced by one
                    // of this topic's own type.
                    if (impl_->participant->delete_topic(ts.data_topic) != RETCODE_OK)
                        throw PubSubError(PubSubStatus::kTransportFailure,
                                          "FastDDS: failed to replace the data topic of '" + name +
                                              "' for payload bound " + std::to_string(bound));
                    ts.data_topic = nullptr;
                }
                if (!ts.data_topic) {
                    ts.data_topic = impl_->participant->create_topic(
                        name, impl_->DataTypeNameFor(bound), TOPIC_QOS_DEFAULT);
                    if (!ts.data_topic)
                        throw PubSubError(PubSubStatus::kTransportFailure,
                                          "FastDDS: failed to create topic: " + name);
                }

                // Created here rather than on first Publish, so the publish path never upgrades
                // its lock: the profile named after this topic if the registry has one, else the
                // Publisher's own default QoS (internal/profile_document.hpp) -- or, with
                // `options.profile` set, that profile by name. The cost is the writer's pool
                // reserved up front for every declared topic, whether or not it is ever published
                // to: at the built-in defaults (max_samples 25, 64 KiB payload bound, data_sharing
                // AUTO) each declared topic reserves roughly 1.6 MB of data-sharing segment plus
                // its payload pool at CreateTopic, published to or not.
                const DataWriterQos wqos =
                    internal::ResolveDataWriterQos(*impl_->publisher, name, options.profile);
                ts.published.data_writer = impl_->publisher->create_datawriter(
                    ts.data_topic, wqos, &impl_->data_writer_listener,
                    // Only the statuses DataWriterListener implements (`<<` is how StatusMask
                    // composes; plain `|` decays to the std::bitset it derives from).
                    // on_unacknowledged_sample_removed has no mask bit and is dispatched whenever a
                    // listener is set.
                    StatusMask::publication_matched()
                        << StatusMask::offered_deadline_missed()
                        << StatusMask::offered_incompatible_qos() << StatusMask::liveliness_lost());
                if (!ts.published.data_writer)
                    throw PubSubError(PubSubStatus::kTransportFailure,
                                      "FastDDS: failed to create DataWriter for: " + name);
                ts.published.payload_bytes = bound;
                ts.published.profile = options.profile;
            }
        }

        // Announce the schema on the companion __schema channel so that
        // late-joining subscribers — and a subscriber-first reader already waiting
        // on this provider — receive it via TRANSIENT_LOCAL.
        if (schema) {
            std::vector<uint8_t> ipc = SerializeSchemaIpc(schema.get());

            if (ts.published.schema_writer) {
                // A publisher already announced a schema for this topic. Idempotent
                // for an identical schema (fan-in / re-declaration); a different one
                // is a genuine conflict that must not be silently dropped. Compared against the
                // bytes that announcement sent, not against a fresh encode of the stored schema.
                if (ipc != ts.published.schema_ipc) {
                    throw PubSubError(
                        PubSubStatus::kSchemaConflict,
                        "FastDDS: topic already declared with a conflicting schema: " + name);
                }
                return;
            }

            // First schema announcement (possibly attaching to a topic state a
            // subscriber-first reader already created).
            std::string schema_topic_name = name + "/__schema";
            // The __schema topic may already exist (a subscriber-first reader
            // created it to await the schema); reuse it.
            if (!ts.schema_topic) {
                ts.schema_topic = impl_->participant->create_topic(
                    schema_topic_name, kSchemaTypeName, TOPIC_QOS_DEFAULT);
                if (!ts.schema_topic)
                    throw PubSubError(
                        PubSubStatus::kTransportFailure,
                        "FastDDS: failed to create schema topic: " + schema_topic_name);
            }

            ts.published.schema_writer = impl_->publisher->create_datawriter(
                ts.schema_topic, internal::MakeSchemaChannelWriterQos());
            if (!ts.published.schema_writer)
                throw PubSubError(
                    PubSubStatus::kTransportFailure,
                    "FastDDS: failed to create schema DataWriter for: " + schema_topic_name);

            // The schema rides the same envelope as a data-channel row: the IPC bytes as the row,
            // plus one attachment carrying the payload bound, through the writer's inherited
            // FletcherSamplePubSubType::serialize.
            const PubSubProvider::RowEncoder encoder = [&ipc](WriteBuffer& b) {
                b.Append(ipc.data(), ipc.size());
            };
            // The announcement carries THIS TOPIC's bound (`ts.published.payload_bytes`, not the
            // provider's own `max_payload_bytes`), so a subscriber can size its reader from it.
            std::vector<uint8_t> bound_bytes(sizeof(uint32_t));
            std::memcpy(bound_bytes.data(), &ts.published.payload_bytes, sizeof(uint32_t));
            Attachments announced;
            announced.Set(kSchemaPayloadBoundKey, Blob(std::move(bound_bytes)));
            internal::PublishData transport;
            transport.encoder = &encoder;
            transport.attachments = &announced;
            // The one write whose failure is invisible from the outside: subscribers learn the
            // schema only from this sample, so a dropped one leaves every subscriber of this topic
            // waiting forever on a future that never resolves.
            const ReturnCode_t rc = ts.published.schema_writer->write(&transport);
            if (rc != RETCODE_OK || !transport.serialize_error.empty()) {
                // Undo the half-announcement before throwing. The caller is being told to retry,
                // and a retry short-circuits on a non-null schema_writer — so one left behind here
                // turns every later CreateTopic for this topic into a silent no-op and makes the
                // failure permanent, which is the very thing the paragraph above says must not
                // happen.
                impl_->publisher->delete_datawriter(ts.published.schema_writer);
                ts.published.schema_writer = nullptr;
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to announce the schema for: " + name +
                                      (transport.serialize_error.empty()
                                           ? ""
                                           : " (" + transport.serialize_error + ")") +
                                      " (schema is " + std::to_string(ipc.size()) +
                                      " bytes; the schema channel's bound is " +
                                      std::to_string(kSchemaPayloadBytes) +
                                      ", of which 37 frame the row and its bound attachment)");
            }

            // Recorded only once the announcement is out, so a failed one leaves nothing behind for
            // a retry to match against and nothing for it to short-circuit on. The OwnedSchema
            // parameter itself is not kept past this point: nothing in this provider needs it
            // again once its bytes are on the wire, and a subscription on this same instance
            // starts from those same bytes (EnsureSchemaChannel).
            ts.published.schema_ipc = std::move(ipc);
        }
    });
}

void FastDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    const RowEncoder& encoder, const Attachments& attachments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number.
    TranslateSeamFailure([&] {
        // The door, before any lock: a Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Publish");

        // Reused per thread: the joined name is only a lookup key and dies with the call, and a
        // fresh std::string here was a malloc and a free on every publish. Publish holds the mutex
        // shared, so a scratch buffer on the provider would be a data race; one per thread is not.
        // This one instance is shared by every `FastDDSPubSubProvider` on this thread, not scoped
        // to `this` — safe only because nothing below reads `name` again once `WriteSample` is
        // called, so a nested Publish on another instance overwriting it on the way back out
        // changes nothing this call still looks at.
        static thread_local std::string name;
        internal::JoinSegmentsInto(name, topic_segments);

        // Shared, so publishes to different topics run concurrently. Held for the whole call: the
        // shared lock keeps the topic and its writer -- created by CreateTopic -- alive underneath
        // the write.
        std::shared_lock lock(impl_->mu);

        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end() || !it->second.published.data_writer)
            throw PubSubError(
                PubSubStatus::kTopicNotDeclared,
                "FastDDS: topic not declared for publishing on this instance: " + name);

        auto& ts = it->second;

        internal::WriteSample(ts.published.data_writer, encoder, attachments);
    });
}

SubscriptionResult FastDDSPubSubProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                    SubscribeCallback callback) {
    return SubscribeWithOptions(topic_segments, std::move(callback), TopicOptions{});
}

SubscriptionResult FastDDSPubSubProvider::SubscribeWithOptions(
    const std::vector<std::string>& topic_segments, SubscribeCallback callback,
    const TopicOptions& options) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number.
    return TranslateSeamFailure([&]() -> SubscriptionResult {
        // The door, before any lock: a Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "SubscribeWithOptions");

        // A subscription follows whatever bound its publisher announces on `__schema`; it never
        // carries one of its own.
        if (options.max_payload_bytes != 0) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "FastDDS: a subscription follows the bound its publisher announces on __schema; "
                "max_payload_bytes may not be set on Subscribe");
        }

        std::string name = internal::JoinSegments(topic_segments);

        // Resolved (and discarded) here, before any lock, so an unknown profile name is refused
        // synchronously rather than surfacing later when the schema thread opens the reader.
        if (!options.profile.empty()) {
            static_cast<void>(
                internal::ResolveDataReaderQos(*impl_->subscriber, name, options.profile));
        }

        std::unique_lock lock(impl_->mu);

        auto& entry = *impl_->topics.try_emplace(name).first;
        auto& ts = entry.second;
        if (ts.subscribed.data_listener)
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "FastDDS: already subscribed to: " + name);

        // The schema channel: opened here, or already open from a SubscribeSchema, in which case
        // its reader keeps serving, and a schema it has already delivered opens this reader right
        // here, no pre-schema buffering at all. Reused rather than replaced — a fresh channel per
        // Subscribe would strand a watch's arrival on a reader nothing feeds any more.
        impl_->EnsureSchemaChannel(entry);

        // Copying is the listener Subscribe installs: LoanedDataReaderListener stays compiled and
        // unit-tested (data_reader_listener.hpp) but is not selected here -- this is the one line
        // that would flip it, behind CanLoanSamples(rqos).
        auto listener = std::make_unique<internal::CopyingDataReaderListener>(
            impl_->status_listener, DeliveryChannel(this, std::move(callback)));

        // `subscribed.data_listener` is written, and `subscribed.schema` read, under `schema_mu`
        // -- the lock HandleSchema holds for its own read of one and write of the other -- so
        // exactly one of "Subscribe finds the schema already known" and "HandleSchema resolves it
        // later" ever opens this reader. OpenDataReader runs while still holding the lock, as
        // HandleSchema's does; the Fast DDS calls it makes do not call back into this provider's
        // own code.
        bool failed = false;
        std::string failure;
        {
            std::lock_guard schema_lock(impl_->schema_mu);
            ts.subscribed.data_listener = std::move(listener);
            ts.subscribed.profile = options.profile;
            if (ts.subscribed.schema) {
                // Broad on purpose, as on the schema thread: the announced bound sizes the
                // reader's pool, so a huge one can fail allocation inside create_datareader. The
                // reason is carried out rather than swallowed -- it is the only thing that tells
                // a too-large announced bound from a topic already held at another type.
                try {
                    impl_->OpenDataReader(name, ts);
                } catch (const std::exception& e) {
                    ts.subscribed.data_listener.reset();
                    failed = true;
                    failure = e.what();
                } catch (...) {
                    ts.subscribed.data_listener.reset();
                    failed = true;
                    failure = "non-std exception";
                }
            }
        }
        if (failed) {
            // Schema side goes with it if nothing else holds it open; CloseSchemaSide takes
            // `schema_mu` itself, hence outside the scope above.
            DataReader* schema_reader =
                ts.subscribed.watches == 0 ? impl_->CloseSchemaSide(ts) : nullptr;
            lock.unlock();
            if (schema_reader) {
                impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
                impl_->subscriber->delete_datareader(schema_reader);
            }
            throw PubSubError(
                PubSubStatus::kTransportFailure,
                "FastDDS: failed to open the data reader for: " + name + " (" + failure + ")");
        }

        return {ts.subscribed.arrival};
    });
}

void FastDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number.
    TranslateSeamFailure([&] {
        // The door, before any lock. Issued from inside a delivery on this instance, the stop+join
        // below waits for the very delivery this thread is executing — a self-wait, and a hang
        // under the suite's TIMEOUT. Refused by name instead -- as are the other three, at their
        // own doors above.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Unsubscribe");

        std::string name = internal::JoinSegments(topic_segments);

        DataReader* data_reader = nullptr;
        DataReader* schema_reader = nullptr;
        // The listener is taken out of the topic state here, under the lock, with the reader that
        // uses it, if one was opened yet, and destroyed at the end of this function — after both
        // readers are gone (Fast DDS's delete_datareader waits for any in-flight on_data_available
        // on `data_reader` first, so the listener it calls into must still be alive for that
        // wait).
        std::unique_ptr<internal::DataReaderListenerBase> listener;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->topics.find(name);
            if (it == impl_->topics.end()) return;

            auto& ts = it->second;
            {
                // `subscribed.data_reader` is READ under `schema_mu` as well as written: the
                // schema thread writes it too (OpenDataReader), so a read taken outside this lock
                // can miss a reader opened the instant this call ran -- leaving it alive,
                // delivering into the listener this function destroys.
                std::lock_guard schema_lock(impl_->schema_mu);
                data_reader = ts.subscribed.data_reader;
                ts.subscribed.data_reader = nullptr;
                listener = std::move(ts.subscribed.data_listener);
                ts.subscribed.profile.clear();
            }
            // Schema side goes with the last user of it.
            schema_reader = ts.subscribed.watches == 0 ? impl_->CloseSchemaSide(ts) : nullptr;
        }

        // Outside `impl_->mu`: a topic other than this one stays available to Publish while a
        // (possibly slow) detach or deletion runs.
        if (schema_reader) {
            impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
            impl_->subscriber->delete_datareader(schema_reader);
        }
        // Fast DDS waits for any in-flight listener callback on `data_reader` before returning.
        if (data_reader) impl_->subscriber->delete_datareader(data_reader);
        // `listener` dies here, at the end of scope, after both readers are gone.
    });
}

SchemaArrival FastDDSPubSubProvider::SubscribeSchema(
    const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number.
    return TranslateSeamFailure([&]() -> SchemaArrival {
        // The door, before any lock, as at the four doors above: a Fast DDS listener callback runs
        // with the RTPS reader mutex held, and the create_datareader below HANGS if it is let
        // through. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "SubscribeSchema");

        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        // Idempotent per topic: EnsureSchemaChannel opens nothing a Subscribe or an earlier watch
        // has already opened, and the count is what makes the channel outlive a data Unsubscribe.
        // Incremented only after the channel exists, so a throwing EnsureSchemaChannel cannot leave
        // a watch counted on a topic with no channel.
        auto& entry = *impl_->topics.try_emplace(name).first;
        auto& ts = entry.second;
        impl_->EnsureSchemaChannel(entry);
        ++ts.subscribed.watches;
        return ts.subscribed.arrival;
    });
}

void FastDDSPubSubProvider::UnsubscribeSchema(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number.
    TranslateSeamFailure([&] {
        // The door, before any lock. Issued from inside a delivery on this instance, the stop+join
        // below waits for the very delivery this thread is executing — a self-wait, and a hang
        // under the suite's TIMEOUT.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "UnsubscribeSchema");

        std::string name = internal::JoinSegments(topic_segments);

        DataReader* schema_reader = nullptr;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->topics.find(name);
            if (it == impl_->topics.end()) return;

            auto& ts = it->second;
            if (ts.subscribed.watches == 0) return;
            if (--ts.subscribed.watches > 0) return;
            // A live data subscription shares the channel -- its reader open yet or not -- and it
            // goes when that is unsubscribed. Decrementing the count to zero is the whole of this
            // call then.
            if (ts.subscribed.data_listener) return;

            // Nothing else remains on this topic: the schema side goes with the last watch.
            schema_reader = impl_->CloseSchemaSide(ts);
        }

        // Outside `impl_->mu`, like Unsubscribe: a topic other than this one stays available to
        // Publish while a (possibly slow) detach or deletion runs.
        if (schema_reader) {
            impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
            impl_->subscriber->delete_datareader(schema_reader);
        }
    });
}

}  // namespace fletcher
