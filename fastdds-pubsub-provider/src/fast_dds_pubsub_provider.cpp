// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Targets eProsima Fast DDS 3.4.x (fast-dds/3.4.0 from Conan Center). Comments across this
// provider cite upstream by file and symbol against that tree — no line numbers, which a point
// release invalidates without anything noticing.
//
// This file holds the provider itself: the pimpl, the participant/publisher/subscriber lifecycle,
// and the four PubSubProvider methods.
//
// Reader side, "hybrid" shape (owner decision 2026-09-15; see the comment on Impl::HandleSchema):
// data delivery is a Fast DDS DataReaderListener again, one per subscribed topic, so
// on_data_available runs inside Fast DDS's own accept path and a same-process reader's history can
// never fill out from under an asynchronous consumer. Schema handling -- the __schema reader's
// take loop, and enabling a topic's data reader once its schema is known -- runs on ONE thread this
// provider owns for its whole life (Impl::schema_thread / Impl::SchemaLoop / Impl::HandleSchema),
// serving every subscribed topic's schema reader off a single WaitSet. Everything they compose
// lives one per header in internal/:
//
//   profile_document.hpp      the participant anchor and the per-endpoint resolution against Fast
//                             DDS's own profile registry -- the one path every document (including
//                             Fletcher's baked-in one) travels
//   qos_defaults.hpp          Fletcher's baked-in Fast DDS XML profiles document -- what an empty
//                             `config.document` resolves to, verbatim from the README
//   fletcher_sample.hpp       the plain sample Fast DDS lends at both ends: its layout, and what
//                             makes it plain for a given bound
//   transport_data.hpp        the sample types handed to Fast DDS on the serialising paths
//   envelope_codec.hpp        the contents of a sample's body
//   fletcher_sample_pub_sub_type.hpp
//                             the data-channel TopicDataType over that layout — and what it
//                             reports about itself, which is what Fast DDS gates data-sharing and
//                             loans on; the companion __schema channel's TopicDataType is the same
//                             class, under a different registered name
//   data_reader_listener.hpp  the data reader side: DataReaderListenerBase (statuses forwarded,
//                             on_data_available -> Drain) and its two Drain bodies,
//                             LoanedDataReaderListener / CopyingDataReaderListener. Copying is the
//                             listener Subscribe installs (owner decision 2026-09-14); Loaned stays
//                             compiled and unit-tested, behind the same CanLoanSamples predicate,
//                             not wired in
//   sample_writer.hpp         the publish flow, SampleWriter; LoanableSampleWriter also lives here,
//                             unit-tested, not selected (owner decision 2026-09-15)
//   data_writer_listener.hpp  the writer statuses forwarded and the writer's status mask
//   participant_listener.hpp  discovery of remote entities, forwarded
//   status_endpoint.hpp       what every forwarding listener shares: a Fast DDS endpoint
//                             translated into the public FastDDSStatusListener::Endpoint
//   schema_channel.hpp        the __schema handoff: a per-subscription promise
//
// No Arrow C++ dependency anywhere in the path: rows arrive as encoded bytes and leave as encoded
// bytes.

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

#include <chrono>
#include <cstdint>
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
#include <fletcher/pubsub/schema_ipc.hpp>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
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
#include "internal/schema_channel.hpp"
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
        Topic* data_topic = nullptr;
        DataWriter* data_writer = nullptr;
        // Companion schema topic (publisher side).
        Topic* schema_topic = nullptr;
        DataWriter* schema_writer = nullptr;
        // Publisher-side schema (nanoarrow), set by CreateTopic when THIS provider declared it.
        OwnedSchema published_schema;
        // The schema as Arrow IPC bytes, publisher side: set by CreateTopic (~817) when this
        // provider publishes the topic, and what a later announcement is byte-compared against
        // (~754). Written and read only under `impl_->mu`.
        std::vector<uint8_t> published_schema_ipc;

        // Companion schema channel (subscriber side): a persistent reader that resolves
        // `schema_channel` when the schema arrives -- so Subscribe works subscriber-first (before
        // any publisher), and SubscribeSchema works with no data reader at all. Opened by
        // EnsureSchemaChannel; one per topic, shared by a watch and the data subscription.
        DataReader* schema_reader = nullptr;
        std::shared_ptr<internal::SchemaChannel> schema_channel;
        // The schema as Arrow IPC bytes, subscriber side: the bytes HandleSchema decoded this
        // topic's schema from, on the schema thread -- what a later sample off `schema_reader` is
        // byte-compared against there. Kept apart from `published_schema_ipc` above, and written
        // only under `schema_mu`: the two DO run on the same TopicState, not different ones, when
        // this provider both publishes and subscribes to the same topic (its own schema_reader
        // takes its own CreateTopic's announcement) -- one field written from both `impl_->mu` and
        // `schema_mu` with no lock in common was a real data race.
        std::vector<uint8_t> received_schema_ipc;
        // How many outstanding SubscribeSchema watches share this topic's channel. While this is
        // non-zero the channel (and its schema reader) outlives the data subscription: Unsubscribe
        // leaves them in the slot instead of tearing them down, and only the matching count of
        // UnsubscribeSchema calls ends it. A count rather than a flag, so two independent watchers
        // cannot end each other's.
        uint32_t schema_watches = 0;

        // Data DataReader (subscriber side). Created DISABLED (SubscriberQos::entity_factory,
        // construction below) with `data_listener` (below) installed, and enabled once the schema
        // is known: on the spot, by Subscribe, if `received_schema` is already set, else by this
        // provider's one schema thread's HandleSchema when it arrives.
        DataReader* data_reader = nullptr;
        // The Fast DDS listener this topic's data reader delivers through -- installed at
        // create_datareader time, so `on_data_available` never fires before `enable()` does.
        // Copying is the listener Subscribe builds (owner decision 2026-09-14); see
        // internal/data_reader_listener.hpp.
        std::unique_ptr<internal::DataReaderListenerBase> data_listener;
        // The schema this topic's data reader delivers with: resolved once, from whichever side
        // got there first -- the publisher's own schema, deep-copied (EnsureSchemaChannel), or
        // the first sample HandleSchema decodes off the schema reader. Kept apart from
        // `published_schema` above (an OwnedSchema, nanoarrow's own type, set only on the publisher
        // side) rather than merged with it: the two have different types and different lifetimes,
        // and a topic this provider both publishes AND subscribes to needs both at once. Handed to
        // `data_listener` via SetSchema before `data_reader->enable()` -- see the comment on
        // Impl::schema_mu below for why every write to this field is made under that lock.
        SharedSchema received_schema;
    };

    // Non-owning and possibly null: the application's observer for endpoint and discovery status
    // (public header). Handed to every listener this provider creates; null means nothing is
    // observed, which is what construction from a `ProviderConfig` alone gives.
    FastDDSStatusListener* status_listener = nullptr;

    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;
    TypeSupport data_type_support;
    TypeSupport schema_type_support;
    // Shared for Publish, exclusive for everything that mutates `topics` or the endpoints in it.
    // DataWriter::write is itself thread safe, so a shared lock is enough to keep the topic and its
    // writer alive for the duration of the call, and publishes to different topics then run
    // concurrently instead of serialising on this mutex. See README "Measured decisions".
    //
    // Lock order: `mu` -> `schema_mu`. The schema thread never takes `mu`. Both `mu` and
    // `schema_mu` are held across a Fast DDS call at points below (`create_datareader`, `enable`,
    // `attach_condition`/`detach_condition`) -- safe because none of those calls synchronously
    // calls back into this provider's own code (the readers they touch either have no listener at
    // all, or one whose callbacks never take either lock -- DataReaderListenerBase,
    // internal/data_reader_listener.hpp), so there is no cycle back into either mutex.
    std::shared_mutex mu;
    // Unordered because Publish looks a topic up by name on every sample and a hash beats the
    // std::map this was: log-n string comparisons per publish bought nothing. Never erased, which
    // is what lets the schema thread hold a bare `TopicState*` in `schema_registry` for as long as
    // that entry is registered without it dangling.
    std::unordered_map<std::string, TopicState> topics;

    // The registered type is built from this one number.
    uint32_t max_payload_bytes = 0;

    // Publish always writes through the regular (serialising) flow now (owner decision
    // 2026-09-15) -- LoanableSampleWriter stays in the tree, unit-tested, but unselected.
    // Stateless, so one instance serves every topic and every thread.
    internal::SampleWriter data_sample_writer;

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
    // `schema_registry` maps a schema reader's StatusCondition to the TopicState it belongs to;
    // EnsureSchemaChannel inserts an entry (under `schema_mu`) before attaching that condition to
    // `schema_wait_set`, so a wake that fires the instant the condition attaches can never find the
    // entry missing. Unsubscribe/UnsubscribeSchema erase it the same way, before detaching.
    //
    // `schema_mu` also guards every write to a TopicState field HandleSchema reads --
    // `data_reader`, `schema_reader`, `received_schema` -- so an application thread nulling one of
    // them (Unsubscribe, UnsubscribeSchema, Subscribe's own unwind paths) can never race the schema
    // thread's read of the same field with no lock of its own to serialise against it. Data
    // readers are otherwise not this thread's concern: their statuses and samples are delivered by
    // their own DataReaderListenerBase (internal/data_reader_listener.hpp), on whichever thread
    // Fast DDS invokes it on.
    eprosima::fastdds::dds::WaitSet schema_wait_set;
    eprosima::fastdds::dds::GuardCondition schema_thread_stop;
    std::mutex schema_mu;
    std::unordered_map<const eprosima::fastdds::dds::Condition*, TopicState*> schema_registry;
    std::thread schema_thread;

    // Reads and forwards every status `reader` (a SCHEMA DataReader only now -- a data reader's
    // statuses are forwarded by its own DataReaderListenerBase, internal/data_reader_listener.hpp)
    // might have changed, via status_endpoint.hpp's ReaderEndpoint, in the fixed order below (the
    // ddsbus WaitsetDataReader::listen pattern this predates). The return value is unused by
    // HandleSchema below -- every call there drains regardless, whether the wake was a real one or
    // the timeout sweep's synthetic poll -- and kept only because the mapping is otherwise
    // byte-for-byte what it always was. Every `get_*_status` call below is read-and-clear per DDS,
    // so each runs at most once per call, REGARDLESS of whether `status_listener` is set: skipping
    // a changed status's getter when there is nobody to forward it to would leave it "changed"
    // forever. Argument mapping copied from the now-deleted DataReaderStatusListener
    // (data_writer_listener.hpp git history) -- that was the spec.
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

    // Runs `ts`'s schema wake: forwards its schema reader's statuses (DispatchReaderStatuses
    // above), then drains it (HandleSchema below), regardless of what the dispatch reported --
    // matching the RETCODE_TIMEOUT sweep, which has no per-topic "did anything change" bit to
    // consult and must drain every registered topic anyway. Caller holds `schema_mu`; a throw
    // stays scoped to this one topic, so a schema-side failure on one topic never skips another's
    // wake in the same pass.
    void RunSchemaWake(TopicState& ts) {
        try {
            bool has_data = DispatchReaderStatuses(ts.schema_reader);
            if (has_data) {
                HandleSchema(ts);
            }
        } catch (const std::exception& ex) {
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA, "reading a schema sample threw: " << ex.what());
        } catch (...) {
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                               "reading a schema sample threw a non-std exception");
        }
    }

    // This provider's one schema thread: every subscribed topic's __schema reader, off one
    // WaitSet. Holds `schema_mu` for the whole pass, including the user code HandleSchema runs
    // through SchemaChannel::Resolve/Fail -- which is what keeps a Subscribe/Unsubscribe on this
    // same topic (they also take `schema_mu`, see its comment above) from observing a half-updated
    // TopicState.
    void SchemaLoop() {
        for (;;) {
            ConditionSeq active;
            // Bounded, not c_TimeInfinite: on two P-cores a status-condition wakeup was missed
            // under a flat-out publisher, leaving a schema reader's own wake asleep with an
            // already-matched data reader still disabled behind it (bench_e2e, Fast DDS 3.4.0).
            // RETCODE_TIMEOUT below sweeps every registered topic as a fallback -- one empty
            // take_next_sample per subscribed topic per 200 ms -- while a real wakeup still
            // short-circuits through the RETCODE_OK path exactly as before.
            const ReturnCode_t wait_rc = schema_wait_set.wait(active, c_TimeInfinite);
            if (schema_thread_stop.get_trigger_value()) return;

            // Tolerant wait (ddsbus WaitsetDataReader::listen): a non-OK return means `active`
            // cannot be trusted, not that this thread should stop -- go back and wait again.
            if (wait_rc != RETCODE_OK) continue;
            std::lock_guard<std::mutex> lock(schema_mu);
            for (Condition* c : active) {
                // Lookup by pointer value only -- never dereferenced before this. A miss is a
                // topic torn down between the wake and this loop (Unsubscribe/UnsubscribeSchema
                // erase the entry before detaching, see their comments), or `schema_thread_stop`'s
                // own condition alongside a real one in the same wake -- either way, nothing to do.
                auto it = schema_registry.find(c);
                if (it != schema_registry.end()) RunSchemaWake(*it->second);
            }
        }
    }

    // Drains `ts.schema_reader`. Called only from the schema thread, with `schema_mu` held --
    // which is also what makes reading `ts.data_reader` here safe: every application-thread write
    // to it is made under the same lock (the comment on `schema_mu` above).
    //
    // The first valid, attachment-free, decodable sample resolves `ts.schema_channel` and -- if a
    // data reader is already waiting on this topic and still disabled -- hands it the schema and
    // enables it right here: the same thing Subscribe does when the schema is already known when
    // it runs, just on this thread instead. Every later sample is only compared against the
    // first's bytes (kept in `ts.received_schema_ipc`): a resend of the same schema is silent, a
    // genuine conflict is a diagnostic only -- CreateTopic already owns refusal for a conflict
    // raised locally. Schema history is one sample deep (MakeSchemaChannelReaderQos), so there is
    // at most one to take per call; no per-sample stop check is needed the way a longer drain would
    // want.
    void HandleSchema(TopicState& ts) {
        internal::ReceivedData sample;
        SampleInfo info;
        ReturnCode_t rc;
        while ((rc = ts.schema_reader->take_next_sample(&sample, &info)) == RETCODE_OK) {
            if (!info.valid_data) continue;

            // The schema rides as a row with no attachments (design owner-approved 2026-09-14);
            // any that arrived is a malformed sample, on the same footing as one that will not
            // decode -- terminal, since a later valid sample cannot retract this one.
            if (!sample.decoded_attachments.empty()) {
                EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                   "ignoring a schema sample that carries attachments: the schema "
                                   "channel only ever sends a row");
                ts.schema_channel->Fail(
                    PubSubStatus::kInternal,
                    "FastDDS: a schema sample carried attachments; the schema channel "
                    "carries a bare row");
                return;
            }

            if (ts.received_schema) {
                // A resend of the same schema (fan-in) is silent; a genuine conflict is
                // diagnostic-only here -- CreateTopic already owns refusal for a local one.
                if (sample.decoded_row != ts.received_schema_ipc) {
                    EPROSIMA_LOG_ERROR(
                        FLETCHER_SCHEMA,
                        "a second publisher announced a different schema for this topic; the "
                        "first announcement stands and this one is ignored");
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
                ts.schema_channel->Fail(
                    PubSubStatus::kInternal,
                    std::string("FastDDS: schema sample would not decode: ") + e.what());
                return;
            } catch (...) {
                EPROSIMA_LOG_ERROR(
                    FLETCHER_SCHEMA,
                    "ignoring a schema sample that will not decode: non-std exception");
                ts.schema_channel->Fail(
                    PubSubStatus::kInternal,
                    "FastDDS: schema sample would not decode: non-std exception");
                return;
            }

            ts.received_schema_ipc = sample.decoded_row;
            ts.received_schema = MakeSharedSchema(std::move(owned));
            // Settles SchemaArrival's own state and wakes any Wait() caller; it runs no user code
            // (SchemaResolver::Resolve, schema_arrival.cpp), so nothing here needs its own
            // try/catch -- a throw from this line is caught by RunSchemaWake's, like everything
            // else in this function.
            ts.schema_channel->Resolve(ts.received_schema);

            if (ts.data_reader && !ts.data_reader->is_enabled()) {
                // SetSchema before enable(), same order Subscribe uses when the schema is already
                // known there: Drain must never run before the listener has one.
                ts.data_listener->SetSchema(ts.received_schema);
                if (ts.data_reader->enable() != RETCODE_OK) {
                    EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                                       "data reader failed to enable after its schema arrived");
                    // A no-op once Resolve above has already consumed the resolver -- reaching for
                    // it anyway is still correct: it is exactly what "the schema arrived but the
                    // data reader itself could not start" would need to report, on the rare path
                    // where Resolve has NOT already run for some other reason.
                    ts.schema_channel->Fail(
                        PubSubStatus::kTransportFailure,
                        "FastDDS: the data reader failed to enable once its schema "
                        "arrived");
                }
            }
        }
        if (rc != RETCODE_NO_DATA) {
            EPROSIMA_LOG_WARNING(FLETCHER_SCHEMA,
                                 "schema reader take_next_sample failed with return code "
                                     << rc << "; the rest of this notification was not read");
        }
    }

    // Opens the topic's schema channel if it has none. Called with `mu` held (shared or
    // exclusive).
    //
    // Resolved on the spot when this provider published the topic -- no reader needed. Otherwise
    // fed by a __schema reader this function creates disabled, registers with the schema thread and
    // enables under the SAME `schema_mu` hold (see the comment at the registration below), then
    // attaches to the waitset only once that `enable()` has actually succeeded -- there is no real
    // Fast DDS trigger to miss before then.
    void EnsureSchemaChannel(TopicState& ts, const std::string& name) {
        if (ts.schema_channel) return;

        if (ts.published_schema) {
            // Publisher-side / cached: nothing to wait for, and no reader to create.
            auto schema_channel = std::make_shared<internal::SchemaChannel>();
            SharedSchema copy = MakeSharedSchema(OwnedSchema::DeepCopy(ts.published_schema.get()));
            schema_channel->Resolve(copy);
            ts.received_schema = std::move(copy);
            ts.schema_channel = std::move(schema_channel);
            return;
        }

        std::string schema_topic_name = name + "/__schema";
        if (!ts.schema_topic) {
            ts.schema_topic = participant->create_topic(
                schema_topic_name, schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
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

        // `ts.schema_channel` is live from here, before `enable()` can possibly trigger a wake:
        // nothing HandleSchema does before it finds an actual sample touches `ts.schema_channel`,
        // but leaving it null through that window is a needless landmine for the next reader of
        // this function.
        ts.schema_reader = schema_reader;
        ts.schema_channel = std::make_shared<internal::SchemaChannel>();

        // Registered and enabled under the SAME `schema_mu` hold: SchemaLoop's own 200 ms TIMEOUT
        // sweep (above) also takes `schema_mu` and walks every registered topic regardless of
        // whether Fast DDS has triggered anything for it, so releasing the lock between the
        // registry insert and `enable()` left a window where that sweep could reach this entry and
        // call `take_next_sample` on a reader `enable()` had not reached yet (logged as a
        // "return code" warning). Attached to the waitset only once `enable()` has actually
        // succeeded -- there is no real Fast DDS trigger to miss before that.
        ReturnCode_t enable_rc;
        {
            std::lock_guard<std::mutex> lock(schema_mu);
            schema_registry[&schema_reader->get_statuscondition()] = &ts;
            enable_rc = schema_reader->enable();
            if (enable_rc != RETCODE_OK) {
                schema_registry.erase(&schema_reader->get_statuscondition());
                ts.schema_reader = nullptr;
                // Nothing has observed `ts.schema_channel->arrival` yet (this function has not
                // returned), so dropping it here needs no Break() -- there is no waiter to release.
                ts.schema_channel.reset();
            }
        }

        if (enable_rc != RETCODE_OK) {
            // Never attached (attach happens only below, after a successful enable()), so there is
            // nothing to detach here.
            subscriber->delete_datareader(schema_reader);
            throw PubSubError(
                PubSubStatus::kTransportFailure,
                "FastDDS: failed to enable schema DataReader for: " + schema_topic_name);
        }

        schema_wait_set.attach_condition(schema_reader->get_statuscondition());
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
            if (ts.schema_reader) subscriber->delete_datareader(ts.schema_reader);
            if (ts.schema_writer) publisher->delete_datawriter(ts.schema_writer);
            if (ts.data_writer) publisher->delete_datawriter(ts.data_writer);
            if (ts.data_reader) subscriber->delete_datareader(ts.data_reader);
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

// The bound `max_payload_bytes == 0` (unset, spec §4.1) resolves to. Bit-for-bit the retired
// options struct's default, and deliberately so: the bound is part of the registered DDS type
// name, so a different number silently stops endpoints discovering each other (locked
// decision 13).
namespace {
constexpr uint32_t kDefaultPayloadBytes = 64 * 1024;
}  // namespace

FastDDSPubSubProvider::FastDDSPubSubProvider(const ProviderConfig& config)
    : FastDDSPubSubProvider(config, nullptr) {}

FastDDSPubSubProvider::FastDDSPubSubProvider(const ProviderConfig& config,
                                             FastDDSStatusListener* status_listener)
    : impl_(std::make_unique<Impl>(status_listener)) {
    // Everything the document decides about the PARTICIPANT — the domain — is settled below,
    // before the participant exists (rung-2, spec §5.1). An empty `config.document` is resolved to
    // Fletcher's baked-in one first (qos_defaults.cpp), so there is exactly one path from here on.
    // Writer/reader QoS is resolved per topic, in `CreateTopic`, the first moment a topic's name is
    // known (internal/profile_document.hpp). Refusals here are `kInvalidArgument`: reached through
    // a factory, a `std::invalid_argument` would arrive at the caller as `kInternal`, which tells
    // an operator nothing.
    const uint32_t bound =
        config.max_payload_bytes == 0 ? kDefaultPayloadBytes : config.max_payload_bytes;
    if (!IsPayloadBound(bound)) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "FastDDS: max_payload_bytes " + std::to_string(bound) +
                              " cannot bound a payload; it must be a multiple of 4 between " +
                              std::to_string(kMinPayloadBytes) + " and " +
                              std::to_string(kMaxPayloadBytes));
    }
    impl_->max_payload_bytes = bound;

    // An empty document IS Fletcher's baked-in one from here on (owner decision 2026-09-15) --
    // there is no more registry-free built-in path, so `document` always ends up loaded into Fast
    // DDS's process-wide profile registry (internal::LoadDocumentOnce, called from
    // ResolveParticipantQos below).
    const std::string document = config.document.empty()
                                     ? std::string(internal::FletcherDefaultProfilesDocument())
                                     : config.document;

    DomainParticipantQos pqos;
    internal::ResolveParticipantQos(document, config.domain_id, pqos);

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

    impl_->data_type_support.reset(new internal::FletcherSamplePubSubType(bound));
    if (impl_->data_type_support.register_type(impl_->participant) != RETCODE_OK)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to register the data type");

    impl_->schema_type_support.reset(new internal::SchemaBytesPubSubType(kSchemaPayloadBytes));
    if (impl_->schema_type_support.register_type(impl_->participant) != RETCODE_OK)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to register the schema type");

    impl_->publisher = impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!impl_->publisher)
        throw PubSubError(PubSubStatus::kTransportFailure, "FastDDS: failed to create Publisher");

    // Every DataReader this subscriber creates is born DISABLED: Subscribe and EnsureSchemaChannel
    // enable each one explicitly, once its topic's schema is known (or, for the schema reader
    // itself, right there in EnsureSchemaChannel). Writers are unaffected: this policy is a
    // SubscriberQos field and only governs entities the SUBSCRIBER creates.
    SubscriberQos sqos = SUBSCRIBER_QOS_DEFAULT;
    sqos.entity_factory().autoenable_created_entities = false;
    impl_->subscriber = impl_->participant->create_subscriber(sqos);
    if (!impl_->subscriber)
        throw PubSubError(PubSubStatus::kTransportFailure, "FastDDS: failed to create Subscriber");

    // The Publisher's/Subscriber's default QoS is already what Fast DDS seeded it with from
    // `document`'s `is_default_profile="true"` profile, if any (else Fast DDS's own default) --
    // done inside create_publisher/create_subscriber above, before this provider touches either.
    // One path always: `document` is never empty by the time it is loaded (see above), so there is
    // no separate "no document" branch here any more.

    // This provider's one schema thread, started once the Subscriber it depends on exists:
    // `schema_thread_stop` attached first, so `schema_wait_set` always has something to wake it
    // for, even before any topic's schema reader does. See Impl::SchemaLoop / Impl::HandleSchema.
    impl_->schema_wait_set.attach_condition(impl_->schema_thread_stop);
    impl_->schema_thread = std::thread([impl = impl_.get()] { impl->SchemaLoop(); });
}

// Destruction precondition (issue #63): no thread may be executing or about to
// enter a public provider API on this instance, and no provider callback may
// still be in flight if that callback can re-enter this provider. Teardown
// invalidates all TopicState pointers; it is NOT a synchronization boundary for
// concurrent use of the provider object.
//
// Teardown deliberately does NOT take impl_->mu (HARD-4 locked decision #4).
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

uint32_t FastDDSPubSubProvider::PayloadBytes() const { return impl_->max_payload_bytes; }

// -----------------------------------------------------------------------
// PubSubProvider interface
// -----------------------------------------------------------------------

void FastDDSPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                        OwnedSchema schema) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, BEFORE any lock (spec §6 clause 6, owner ruling 2026-09-05,
        // "Re-entry is refused on every protocol"). A Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        //
        // "Before any lock" is not decoration: with this check one line lower,
        // after `impl_->mu`, the refusal never runs and the call hangs on the
        // mutex exactly as it did with no door at all. That is a measured
        // mistake, not a hypothetical one.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "CreateTopic");

        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        // Idempotent, mirroring the in-process reference provider
        // (InProcessPubSubProvider::CreateTopic): declaring a topic never fails on an
        // existing one. The topic state may already exist because a subscriber
        // joined first (subscriber-first) and lazily created it without a schema,
        // or because a publisher already declared it. Attach the publisher side
        // and announce the schema exactly once.
        auto& ts = impl_->topics[name];

        // The data topic may already exist (created by a prior Subscribe); reuse it.
        if (!ts.data_topic) {
            ts.data_topic = impl_->participant->create_topic(
                name, impl_->data_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
            if (!ts.data_topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create topic: " + name);
        }

        // Created here rather than on first Publish, so the publish path never upgrades its lock:
        // the profile named after this topic if the registry has one, else the Publisher's own
        // default QoS (internal/profile_document.hpp). The cost is the writer's pool reserved up
        // front for every declared topic, whether or not it is ever published to: at the built-in
        // defaults (max_samples 100, 64 KiB payload bound, data_sharing AUTO) each declared topic
        // reserves roughly 6.6 MB of data-sharing segment plus its payload pool at CreateTopic,
        // published to or not.
        if (!ts.data_writer) {
            const DataWriterQos wqos = internal::ResolveDataWriterQos(*impl_->publisher, name);
            ts.data_writer = impl_->publisher->create_datawriter(
                ts.data_topic, wqos, &impl_->data_writer_listener, internal::WriterStatusMask());
            if (!ts.data_writer)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create DataWriter for: " + name);
        }

        // Announce the schema on the companion __schema channel so that
        // late-joining subscribers — and a subscriber-first reader already waiting
        // on this provider — receive it via TRANSIENT_LOCAL.
        if (schema) {
            std::vector<uint8_t> ipc = SerializeSchemaIpc(schema.get());

            if (ts.schema_writer) {
                // A publisher already announced a schema for this topic. Idempotent
                // for an identical schema (fan-in / re-declaration); a different one
                // is a genuine conflict that must not be silently dropped. Compared against the
                // bytes that announcement sent, not against a fresh encode of the stored schema.
                if (ts.published_schema && ipc != ts.published_schema_ipc) {
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
                    schema_topic_name, impl_->schema_type_support.get_type_name(),
                    TOPIC_QOS_DEFAULT);
                if (!ts.schema_topic)
                    throw PubSubError(
                        PubSubStatus::kTransportFailure,
                        "FastDDS: failed to create schema topic: " + schema_topic_name);
            }

            ts.schema_writer = impl_->publisher->create_datawriter(
                ts.schema_topic, internal::MakeSchemaChannelWriterQos());
            if (!ts.schema_writer)
                throw PubSubError(
                    PubSubStatus::kTransportFailure,
                    "FastDDS: failed to create schema DataWriter for: " + schema_topic_name);

            // The schema rides the same envelope as a data-channel row -- a row with no
            // attachments -- through the writer's inherited FletcherSamplePubSubType::serialize.
            const PubSubProvider::RowEncoder encoder = [&ipc](WriteBuffer& b) {
                b.Append(ipc.data(), ipc.size());
            };
            const Attachments none;
            internal::PublishData transport;
            transport.encoder = &encoder;
            transport.attachments = &none;
            // The one write whose failure is invisible from the outside: subscribers learn the
            // schema only from this sample, so a dropped one leaves every subscriber of this topic
            // waiting forever on a future that never resolves.
            const ReturnCode_t rc = ts.schema_writer->write(&transport);
            if (rc != RETCODE_OK || !transport.serialize_error.empty()) {
                // Undo the half-announcement before throwing. The caller is being told to retry,
                // and a retry short-circuits on a non-null schema_writer — so one left behind here
                // turns every later CreateTopic for this topic into a silent no-op and makes the
                // failure permanent, which is the very thing the paragraph above says must not
                // happen.
                impl_->publisher->delete_datawriter(ts.schema_writer);
                ts.schema_writer = nullptr;
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to announce the schema for: " + name +
                                      (transport.serialize_error.empty()
                                           ? ""
                                           : " (" + transport.serialize_error + ")") +
                                      " (schema is " + std::to_string(ipc.size()) +
                                      " bytes; the schema channel's bound is " +
                                      std::to_string(kSchemaPayloadBytes) +
                                      ", of which 8 frame the row)");
            }

            // Recorded only once the announcement is out, so a failed one leaves nothing behind for
            // a retry to match against and nothing for it to short-circuit on.
            ts.published_schema = OwnedSchema::DeepCopy(schema.get());
            ts.published_schema_ipc = std::move(ipc);
        }
    });
}

void FastDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    const RowEncoder& encoder, const Attachments& attachments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05,
        // "Re-entry is refused on every protocol"). A Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Publish");

        // Reused per thread: the joined name is only a lookup key and dies with the call, and a
        // fresh std::string here was a malloc and a free on every publish. Publish holds the mutex
        // shared, so a scratch buffer on the provider would be a data race; one per thread is not.
        // This one instance is shared by every `FastDDSPubSubProvider` on this thread, not scoped
        // to `this` — safe only because nothing below reads `name` again once `Write` is called, so
        // a nested Publish on another instance overwriting it on the way back out changes nothing
        // this call still looks at.
        static thread_local std::string name;
        internal::JoinSegmentsInto(name, topic_segments);

        // Shared, so publishes to different topics run concurrently. Held for the whole call: the
        // shared lock keeps the topic and its writer -- created by CreateTopic -- alive underneath
        // the write.
        std::shared_lock lock(impl_->mu);

        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end() || !it->second.data_writer)
            throw PubSubError(
                PubSubStatus::kTopicNotDeclared,
                "FastDDS: topic not declared for publishing on this instance: " + name);

        auto& ts = it->second;

        // Always the regular (serialising) flow now (owner decision 2026-09-15); see
        // internal/sample_writer.hpp. Stateless, so one instance serves every topic and every
        // thread.
        impl_->data_sample_writer.Write(ts.data_writer, encoder, attachments);
    });
}

SubscriptionResult FastDDSPubSubProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                    SubscribeCallback callback) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    return TranslateSeamFailure([&]() -> SubscriptionResult {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05,
        // "Re-entry is refused on every protocol"). A Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Subscribe");

        std::string name = internal::JoinSegments(topic_segments);
        std::unique_lock lock(impl_->mu);

        auto& ts = impl_->topics[name];
        if (ts.data_reader)
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "FastDDS: already subscribed to: " + name);

        // Data DDS topic.
        if (!ts.data_topic) {
            ts.data_topic = impl_->participant->create_topic(
                name, impl_->data_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
            if (!ts.data_topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create topic: " + name);
        }

        // The schema channel: opened here, or already open from a SubscribeSchema, in which case
        // its reader keeps serving, and a schema it has already delivered starts this reader off
        // enabled immediately below, no pre-schema buffering at all. Reused rather than replaced —
        // a fresh channel per Subscribe would strand a watch's arrival on a reader nothing feeds
        // any more.
        impl_->EnsureSchemaChannel(ts, name);

        const DataReaderQos rqos = internal::ResolveDataReaderQos(*impl_->subscriber, name);

        // Copying is the listener Subscribe installs (owner decision 2026-09-14):
        // LoanedDataReaderListener stays compiled and unit-tested (data_reader_listener.hpp) but
        // is not selected here -- this is the one line that would flip it, behind
        // CanLoanSamples(rqos).
        ts.data_listener = std::make_unique<internal::CopyingDataReaderListener>(
            impl_->status_listener, DeliveryChannel(this, std::move(callback)));

        // Created DISABLED (SubscriberQos::entity_factory, construction) with `ts.data_listener`
        // installed: on_data_available cannot fire before `enable()` below runs, so there is
        // nothing for the listener to deliver before `SetSchema` has run (below, or from
        // Impl::HandleSchema on the schema thread).
        DataReader* data_reader = impl_->subscriber->create_datareader(
            ts.data_topic, rqos, ts.data_listener.get(), StatusMask::all());
        if (!data_reader) {
            ts.data_listener.reset();
            // Tear down the schema side only if THIS call -- or an earlier one, now abandoned --
            // is what opened it and no watch keeps it.
            const bool keep_watch = ts.schema_watches > 0;
            DataReader* schema_reader = nullptr;
            std::shared_ptr<internal::SchemaChannel> schema_channel;
            if (!keep_watch && ts.schema_reader) {
                schema_reader = ts.schema_reader;
                // `ts.schema_channel`/`schema_reader`/`ts.received_schema` are nulled (and
                // `ts.schema_channel` moved out) under `schema_mu`, not before it: the schema
                // thread could be mid-HandleSchema on this very schema_reader right now, reading
                // them with no lock of its own to race against this write. `ts.received_schema`
                // goes with it -- a later EnsureSchemaChannel on this topic must see "not yet
                // known", not a stale value that makes HandleSchema treat a fresh schema_reader's
                // first sample as an already-resolved resend and skip resolving the fresh channel
                // it belongs to.
                std::lock_guard<std::mutex> schema_lock(impl_->schema_mu);
                impl_->schema_registry.erase(&schema_reader->get_statuscondition());
                ts.schema_reader = nullptr;
                ts.received_schema = nullptr;
                schema_channel = std::move(ts.schema_channel);
            }
            lock.unlock();
            if (schema_reader)
                impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
            if (schema_channel) schema_channel->Break();
            if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
            throw PubSubError(PubSubStatus::kTransportFailure,
                              "FastDDS: failed to create DataReader for: " + name);
        }

        // `ts.data_reader` is written, and `ts.received_schema` read, under `schema_mu` -- the same
        // lock HandleSchema holds for its own write of one and read of the other -- so exactly one
        // of "Subscribe finds the schema already known" and "HandleSchema resolves it later" ever
        // enables this reader, never both and never neither: whichever of the two checks
        // ts.received_schema first (mutual exclusion decides which) sees the true, final answer,
        // and `ts.received_schema` only ever transitions unset -> set, once, so the loser of that
        // race can never find a fresh reason to act. `enable()` runs while still holding the lock,
        // for the same reason HandleSchema's does -- calling into Fast DDS here is fine, it does
        // not call back into this provider's code.
        bool enable_now = false;
        {
            std::lock_guard<std::mutex> schema_lock(impl_->schema_mu);
            ts.data_reader = data_reader;
            if (ts.received_schema) {
                ts.data_listener->SetSchema(ts.received_schema);
                enable_now = true;
            }
        }

        if (enable_now) {
            // Schema already known: enable right here, on this thread -- intraprocess matching
            // (and any TRANSIENT_LOCAL backlog it triggers) then runs synchronously on it.
            // Otherwise the schema thread's HandleSchema does both, once the schema arrives.
            if (data_reader->enable() != RETCODE_OK) {
                std::unique_ptr<internal::DataReaderListenerBase> listener;
                DataReader* schema_reader = nullptr;
                std::shared_ptr<internal::SchemaChannel> schema_channel;
                const bool keep_watch = ts.schema_watches > 0;
                {
                    std::lock_guard<std::mutex> schema_lock(impl_->schema_mu);
                    ts.data_reader = nullptr;
                    listener = std::move(ts.data_listener);
                    if (!keep_watch && ts.schema_reader) {
                        // Nothing else is left on this topic: take the schema side down too.
                        // `enable_now` is also true when EnsureSchemaChannel resolved the schema
                        // from this provider's own published copy and opened no schema reader at
                        // all (CreateTopic then Subscribe on one provider) -- guarded the same way
                        // the sibling unwinds above (~921) and in Unsubscribe (~1046) are.
                        schema_reader = ts.schema_reader;
                        schema_channel = std::move(ts.schema_channel);
                        impl_->schema_registry.erase(&schema_reader->get_statuscondition());
                        ts.schema_reader = nullptr;
                        ts.received_schema = nullptr;
                    }
                }
                lock.unlock();
                if (schema_reader)
                    impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
                impl_->subscriber->delete_datareader(data_reader);
                if (schema_channel) schema_channel->Break();
                if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to enable DataReader for: " + name);
            }
        }

        return {ts.schema_channel->arrival};
    });
}

void FastDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05).
        // Issued from inside a delivery on this instance, the stop+join below waits for the very
        // delivery this thread is executing — a self-wait, and a hang under the suite's TIMEOUT.
        // Refused by name instead -- as are the other three, at their own doors above.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Unsubscribe");

        std::string name = internal::JoinSegments(topic_segments);

        DataReader* data_reader = nullptr;
        DataReader* schema_reader = nullptr;
        // Taken out of the slot ONLY when no watch keeps it (see below); a kept channel is left IN
        // the slot.
        std::shared_ptr<internal::SchemaChannel> schema_channel;
        // The listener is taken out of the topic state here, under the lock, with the reader that
        // uses it, and destroyed at the end of this function — after both readers are gone (Fast
        // DDS's delete_datareader waits for any in-flight on_data_available on `data_reader`
        // first, so the listener it calls into must still be alive for that wait).
        std::unique_ptr<internal::DataReaderListenerBase> listener;
        {
            std::lock_guard<std::shared_mutex> lock(impl_->mu);
            auto it = impl_->topics.find(name);
            if (it == impl_->topics.end()) return;

            auto& ts = it->second;
            data_reader = ts.data_reader;
            const bool keep_watch = ts.schema_watches > 0;
            if (!keep_watch) schema_reader = ts.schema_reader;

            // `ts.data_reader`/`ts.data_listener` and, when no watch keeps the schema side either,
            // `ts.schema_reader`/`ts.received_schema`/`ts.schema_channel` are nulled under
            // `schema_mu`, not before it: the schema thread could be mid-HandleSchema on this very
            // schema_reader right now, reading them with no lock of its own to race against this
            // write. `ts.received_schema` goes with `ts.schema_reader`: a later EnsureSchemaChannel
            // on this topic must see "not yet known", not a stale value that makes HandleSchema
            // treat a fresh schema_reader's first sample as an already-resolved resend and skip
            // resolving the fresh channel it belongs to (this was a real, reproducible bug --
            // ResubscribeAfterUnsubscribeKeepsDelivering hung on exactly this).
            {
                std::lock_guard<std::mutex> schema_lock(impl_->schema_mu);
                ts.data_reader = nullptr;
                listener = std::move(ts.data_listener);
                if (!keep_watch && schema_reader) {
                    impl_->schema_registry.erase(&schema_reader->get_statuscondition());
                    ts.schema_reader = nullptr;
                    ts.received_schema = nullptr;
                    schema_channel = std::move(ts.schema_channel);
                }
            }
        }

        // Outside `impl_->mu`: a topic other than this one stays available to Publish while a
        // (possibly slow) detach or deletion runs.
        if (schema_reader)
            impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
        if (schema_channel) schema_channel->Break();
        // Fast DDS waits for any in-flight listener callback on each reader before returning.
        if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
        if (data_reader) impl_->subscriber->delete_datareader(data_reader);
        // `listener` dies here, at the end of scope, after both readers are gone.
    });
}

SchemaArrival FastDDSPubSubProvider::SubscribeSchema(
    const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    return TranslateSeamFailure([&]() -> SchemaArrival {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05), as at the four
        // doors above: a Fast DDS listener callback runs with the RTPS reader mutex held, and the
        // create_datareader below HANGS if it is let through. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "SubscribeSchema");

        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        // Idempotent per topic: EnsureSchemaChannel opens nothing a Subscribe or an earlier watch
        // has already opened, and the count is what makes the channel outlive a data Unsubscribe.
        // Incremented only after the channel exists, so a throwing EnsureSchemaChannel cannot leave
        // a watch counted on a topic with no channel.
        auto& ts = impl_->topics[name];
        impl_->EnsureSchemaChannel(ts, name);
        ++ts.schema_watches;
        return ts.schema_channel->arrival;
    });
}

void FastDDSPubSubProvider::UnsubscribeSchema(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05). Issued from
        // inside a delivery on this instance, the stop+join below waits for the very delivery
        // this thread is executing — a self-wait, and a hang under the suite's TIMEOUT.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "UnsubscribeSchema");

        std::string name = internal::JoinSegments(topic_segments);

        DataReader* schema_reader = nullptr;
        std::shared_ptr<internal::SchemaChannel> schema_channel;
        {
            std::lock_guard<std::shared_mutex> lock(impl_->mu);
            auto it = impl_->topics.find(name);
            if (it == impl_->topics.end()) return;

            auto& ts = it->second;
            if (ts.schema_watches == 0) return;
            if (--ts.schema_watches > 0) return;
            // A live data subscription shares the channel; it goes when that is unsubscribed.
            // Decrementing the count to zero is the whole of this call then.
            if (ts.data_reader) return;

            // Nothing else remains on this topic: the schema reader comes down too. `schema_mu`,
            // not `mu`, guards the writes below for the same reason Unsubscribe's do -- the schema
            // thread could be mid-HandleSchema on this very schema_reader right now, reading them
            // with no lock of its own to race against this write. `ts.received_schema` goes with
            // `ts.schema_reader`, same reason as Unsubscribe: a later watch or Subscribe on this
            // topic must see "not yet known", not a stale value that makes a fresh schema_reader's
            // first sample look like an already-resolved resend.
            schema_reader = ts.schema_reader;
            std::lock_guard<std::mutex> schema_lock(impl_->schema_mu);
            impl_->schema_registry.erase(&schema_reader->get_statuscondition());
            ts.schema_reader = nullptr;
            ts.received_schema = nullptr;
            schema_channel = std::move(ts.schema_channel);
        }

        // Outside `impl_->mu`, like Unsubscribe: a topic other than this one stays available to
        // Publish while a (possibly slow) detach or deletion runs.
        impl_->schema_wait_set.detach_condition(schema_reader->get_statuscondition());
        if (schema_channel) schema_channel->Break();
        impl_->subscriber->delete_datareader(schema_reader);
    });
}

}  // namespace fletcher
