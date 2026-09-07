// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// One place where a Fast DDS endpoint becomes what the public FastDDSStatusListener speaks. Shared
// by all three listeners that forward statuses (data reader, data writer, schema reader), which is
// why it is a header of its own rather than a member of any of them.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_STATUS_ENDPOINT_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_STATUS_ENDPOINT_HPP_

#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TopicDescription.hpp>
#include <string>
#include <string_view>

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

namespace fletcher {
namespace internal {

// `TopicDescription::get_name()` hands back a reference to the name the topic owns, so a view over
// it outlives the listener call this Endpoint is built for — which is exactly as long as the public
// contract promises it is valid.
inline FastDDSStatusListener::Endpoint MakeEndpoint(const std::string& topic_name, bool is_writer) {
    std::string_view topic(topic_name);
    // The companion channel's DDS name is the Fletcher topic plus this suffix (CreateTopic and
    // Subscribe both build it that way), so stripping it back off is what tells the schema handoff
    // apart from the caller's rows on the public seam.
    constexpr std::string_view kSchemaSuffix = "/__schema";
    const bool is_schema_channel = topic.ends_with(kSchemaSuffix);
    if (is_schema_channel) topic.remove_suffix(kSchemaSuffix.size());
    return {topic, is_schema_channel, is_writer};
}

inline FastDDSStatusListener::Endpoint ReaderEndpoint(eprosima::fastdds::dds::DataReader* reader) {
    return MakeEndpoint(reader->get_topicdescription()->get_name(), false);
}

inline FastDDSStatusListener::Endpoint WriterEndpoint(eprosima::fastdds::dds::DataWriter* writer) {
    return MakeEndpoint(writer->get_topic()->get_name(), true);
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_STATUS_ENDPOINT_HPP_
