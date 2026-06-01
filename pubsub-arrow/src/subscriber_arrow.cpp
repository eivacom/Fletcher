// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub_arrow/subscriber_arrow.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <fletcher/pubsub/owned_schema.hpp>
#include <stdexcept>

namespace fletcher {

namespace {

std::shared_ptr<arrow::Schema> ImportFromNano(const ArrowSchema* schema) {
    if (!schema || !schema->release) {
        return nullptr;
    }
    OwnedSchema copy = OwnedSchema::DeepCopy(schema);
    auto result = arrow::ImportSchema(copy.get());
    if (!result.ok()) {
        throw std::runtime_error("SubscriberArrow: ImportSchema: " + result.status().ToString());
    }
    return *result;
}

}  // anonymous namespace

SubscriberArrow::SubscriberArrow(std::shared_ptr<PubSubProvider> provider)
    : subscriber_(std::make_unique<Subscriber>(std::move(provider))) {}

SubscriberArrow::~SubscriberArrow() = default;

SubscriberArrow::SubscribeResult SubscriberArrow::Subscribe(
    const std::vector<std::string>& segments, SubscribeCallback callback) {
    std::string key = JoinSegments(segments);

    Subscriber::SubscribeResult result = subscriber_->Subscribe(
        segments,
        [this, key, cb = std::move(callback)](uint64_t /*sub_id*/, const uint8_t* data, size_t len,
                                              SharedSchema /*schema*/, Attachments att) {
            Codec* codec;
            {
                std::lock_guard lock(mu_);
                auto it = codecs_.find(key);
                if (it == codecs_.end()) {
                    return;
                }
                codec = it->second.codec.get();
            }
            ArrowRow row = codec->DecodeRow(data, len);
            cb(std::move(row), std::move(att));
        });

    std::shared_ptr<arrow::Schema> arrow_schema;
    if (result.schema) {
        arrow_schema = ImportFromNano(result.schema.get());
        std::lock_guard lock(mu_);
        if (codecs_.find(key) == codecs_.end()) {
            codecs_[key] = TopicCodec{arrow_schema, std::make_unique<Codec>(arrow_schema)};
        }
    }

    // Track sub_id -> topic_key so Unsubscribe can release the codec
    // entry when the last subscription for a topic is removed.
    {
        std::lock_guard lock(mu_);
        sub_topic_[result.subscription_id] = key;
    }

    return {result.subscription_id, std::move(arrow_schema)};
}

void SubscriberArrow::Unsubscribe(uint64_t subscription_id) {
    // First, ask the underlying Subscriber to release the subscription.
    // Then clean up codec state if no subscriptions for this topic remain.
    subscriber_->Unsubscribe(subscription_id);

    std::lock_guard lock(mu_);
    auto it = sub_topic_.find(subscription_id);
    if (it == sub_topic_.end()) {
        return;
    }
    std::string key = std::move(it->second);
    sub_topic_.erase(it);

    bool any_remaining = false;
    for (const auto& [_, topic] : sub_topic_) {
        if (topic == key) {
            any_remaining = true;
            break;
        }
    }
    if (!any_remaining) {
        codecs_.erase(key);
    }
}

std::string SubscriberArrow::JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) {
        return {};
    }
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // namespace fletcher
