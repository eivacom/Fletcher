// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_
#define FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_

#include <arrow/type_fwd.h>

#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fletcher {

/// Arrow-friendly wrapper around Subscriber. Decodes incoming row
/// bytes into ArrowRow via Codec before delivering to the caller.
///
/// Server-side code that works with Arrow C++ types should use
/// SubscriberArrow; edge-deployed code that already speaks the raw
/// row-bytes interface uses Subscriber directly.
class SubscriberArrow {
   public:
    explicit SubscriberArrow(std::shared_ptr<PubSubProvider> provider);
    ~SubscriberArrow();

    SubscriberArrow(const SubscriberArrow&) = delete;
    SubscriberArrow& operator=(const SubscriberArrow&) = delete;

    struct SubscribeResult {
        uint64_t subscription_id;
        std::shared_ptr<arrow::Schema> schema;
    };

    /// Subscribe with ArrowRow delivery.
    using SubscribeCallback = std::function<void(ArrowRow row, Attachments attachments)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments, SubscribeCallback callback);

    void Unsubscribe(uint64_t subscription_id);

   private:
    std::unique_ptr<Subscriber> subscriber_;

    mutable std::mutex mu_;
    struct TopicCodec {
        std::shared_ptr<arrow::Schema> arrow_schema;
        std::unique_ptr<Codec> codec;
    };
    std::unordered_map<std::string, TopicCodec> codecs_;
    // Maps subscription_id -> topic key so Unsubscribe can free codec
    // entries once the last subscription for a topic is gone.
    std::unordered_map<uint64_t, std::string> sub_topic_;

    static std::string JoinSegments(const std::vector<std::string>& segs);
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_
