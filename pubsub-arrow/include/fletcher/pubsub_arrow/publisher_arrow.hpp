// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBLISHER_ARROW_HPP_
#define FLETCHER_INCLUDE_PUBLISHER_ARROW_HPP_

#include <arrow/type_fwd.h>

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fletcher {

/// Arrow-friendly wrapper around Publisher that accepts arrow::Schema
/// and ArrowRow inputs, encoding rows via Codec before forwarding to
/// the underlying Publisher.
///
/// Server-side code that works with Arrow C++ types should use
/// PublisherArrow; edge-deployed code that already speaks the raw
/// row-bytes interface uses Publisher directly.
class PublisherArrow {
   public:
    explicit PublisherArrow(std::shared_ptr<PubSubProvider> provider);
    ~PublisherArrow();

    PublisherArrow(const PublisherArrow&) = delete;
    PublisherArrow& operator=(const PublisherArrow&) = delete;

    /// Create a topic with an Arrow C++ schema.
    /// Passing nullptr is allowed (topic created without schema).
    void CreateTopic(const std::vector<std::string>& segments,
                     std::shared_ptr<arrow::Schema> schema);

    /// Publish an ArrowRow (encoded via Codec).
    void Publish(const std::vector<std::string>& segments, const ArrowRow& row,
                 const Attachments& attachments = {});

    /// Publish using a direct encoder (passthrough to Publisher).
    void PublishDirect(const std::vector<std::string>& segments,
                       PubSubProvider::RowEncoder encoder, const Attachments& attachments = {});

    std::vector<std::string> ListTopics() const;
    bool HasTopic(const std::vector<std::string>& segments) const;

   private:
    std::unique_ptr<Publisher> publisher_;

    mutable std::mutex mu_;
    struct TopicCodec {
        std::shared_ptr<arrow::Schema> arrow_schema;
        std::unique_ptr<Codec> codec;
    };
    std::unordered_map<std::string, TopicCodec> codecs_;

    static std::string JoinSegments(const std::vector<std::string>& segs);
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBLISHER_ARROW_HPP_
