// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The opt-in logging listener: what the provider logged, at the levels it logged it, before the
// statuses became the application's to observe. Each body is the line the listener it replaced
// carried; the reasons for the levels came with them.
//
// A `.cpp` inside the library, so eProsima headers are allowed here — which is the whole point of
// declaring these bodies out of line: the public header stays free of DDS vocabulary and a
// consumer pays nothing for the option.
#include <cstdint>
#include <fastdds/dds/log/Log.hpp>

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

namespace fletcher {

// INFO needs FASTDDS_ENFORCE_LOG_INFO to appear at all (Log.hpp), so the routine half of each pair
// below is compiled out unless the build defines it. Warnings and errors are always compiled in.
//
// Losing a peer is the half worth hearing about: a writer with no readers left keeps accepting
// publishes and delivers them nowhere, and a reader with no writers left receives nothing.
void FastDDSLoggingStatusListener::OnMatched(Endpoint endpoint, int32_t current_count,
                                             int32_t change) noexcept {
    if (endpoint.is_writer) {
        if (change < 0) {
            EPROSIMA_LOG_WARNING(FLETCHER_PUBLICATION, "writer on '"
                                                           << endpoint.topic << "' lost a reader, "
                                                           << current_count << " still matched");
        } else {
            EPROSIMA_LOG_INFO(FLETCHER_PUBLICATION, "writer on '"
                                                        << endpoint.topic << "' matched a reader, "
                                                        << current_count << " now matched");
        }
    } else {
        if (change < 0) {
            EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION, "reader on '"
                                                            << endpoint.topic << "' lost a writer, "
                                                            << current_count << " still matched");
        } else {
            EPROSIMA_LOG_INFO(FLETCHER_SUBSCRIPTION, "reader on '"
                                                         << endpoint.topic << "' matched a writer, "
                                                         << current_count << " now matched");
        }
    }
}

// A QoS mismatch means the endpoints never match, which otherwise shows up only as a subscriber
// that never receives anything. Publish keeps succeeding — there is simply nobody to deliver to.
void FastDDSLoggingStatusListener::OnIncompatibleQos(Endpoint endpoint, uint32_t policy_id,
                                                     uint32_t /*total_count*/) noexcept {
    if (endpoint.is_writer) {
        EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION, "writer on '"
                                                     << endpoint.topic
                                                     << "' rejected by a reader over QoS policy id "
                                                     << policy_id << "; samples are going nowhere");
    } else {
        EPROSIMA_LOG_ERROR(FLETCHER_SUBSCRIPTION,
                           "reader on '" << endpoint.topic
                                         << "' rejected by a writer over QoS policy id "
                                         << policy_id << "; no samples will arrive");
    }
}

// Fletcher sets no DEADLINE, so this only fires on an endpoint an operator gave one to through the
// provider document. Logged so that a configured policy cannot fail silently.
void FastDDSLoggingStatusListener::OnDeadlineMissed(Endpoint endpoint,
                                                    uint32_t total_count) noexcept {
    if (endpoint.is_writer) {
        EPROSIMA_LOG_WARNING(FLETCHER_PUBLICATION,
                             "writer on '" << endpoint.topic << "' missed its offered deadline, "
                                           << total_count << " times in all");
    } else {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << endpoint.topic << "' missed its requested deadline, "
                                           << total_count << " times in all");
    }
}

// Under AUTOMATIC with an infinite lease, not-alive means the writer vanished. The line used to
// branch on the not-alive *delta*; the seam carries counts rather than deltas, so a standing
// not-alive writer keeps this at WARNING instead of dropping back to INFO once reported.
void FastDDSLoggingStatusListener::OnLivelinessChanged(Endpoint endpoint, int32_t alive_count,
                                                       int32_t not_alive_count) noexcept {
    if (not_alive_count > 0) {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << endpoint.topic << "' has " << not_alive_count
                                           << " writer(s) no longer asserting liveliness, "
                                           << alive_count << " still alive");
    } else {
        EPROSIMA_LOG_INFO(FLETCHER_SUBSCRIPTION, "reader on '" << endpoint.topic << "' has "
                                                               << alive_count << " live writer(s)");
    }
}

// Readers have marked this writer NOT_ALIVE and stop expecting its samples. Fletcher leaves
// LIVELINESS at its AUTOMATIC default with an infinite lease (`QosPolicies.hpp`), where it cannot
// fire, so this too reports a policy an operator configured.
void FastDDSLoggingStatusListener::OnLivelinessLost(Endpoint endpoint,
                                                    uint32_t total_count) noexcept {
    EPROSIMA_LOG_WARNING(FLETCHER_PUBLICATION,
                         "writer on '" << endpoint.topic << "' lost liveliness, " << total_count
                                       << " times in all; readers consider it not alive");
}

void FastDDSLoggingStatusListener::OnSampleLost(Endpoint endpoint, uint32_t total_count) noexcept {
    if (endpoint.is_schema_channel) {
        EPROSIMA_LOG_WARNING(FLETCHER_SCHEMA, "a schema sample was lost ("
                                                  << total_count
                                                  << " total); the schema future stays unresolved "
                                                     "until the writer's retained sample arrives");
    } else {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION, "reader on '" << endpoint.topic << "' lost "
                                                                  << total_count << " sample(s)");
    }
}

void FastDDSLoggingStatusListener::OnSampleRejected(Endpoint endpoint, int32_t reason,
                                                    uint32_t total_count) noexcept {
    if (endpoint.is_schema_channel) {
        EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                           "a schema sample was rejected (reason "
                               << reason << ", " << total_count
                               << " total); if it was too large for the channel, raise "
                                  "max_schema_bytes on this endpoint");
    } else {
        EPROSIMA_LOG_WARNING(FLETCHER_SUBSCRIPTION,
                             "reader on '" << endpoint.topic << "' rejected a sample (reason "
                                           << reason << ", " << total_count
                                           << " total); resource limits are too tight");
    }
}

// KEEP_ALL + RELIABLE means the writer blocks rather than drops, so an unacknowledged sample being
// removed is history overflowing under max_blocking_time — data loss, not backpressure.
void FastDDSLoggingStatusListener::OnUnacknowledgedSampleRemoved(Endpoint endpoint) noexcept {
    EPROSIMA_LOG_WARNING(
        FLETCHER_PUBLICATION,
        "writer on '" << endpoint.topic << "' dropped a sample no reader had acknowledged");
}

}  // namespace fletcher
