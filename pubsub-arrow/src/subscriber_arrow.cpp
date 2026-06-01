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
        throw std::runtime_error("SubscriberArrow: ImportSchema: " +
                                 result.status().ToString());
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
        segments, [this, key, cb = std::move(callback)](const uint8_t* data, size_t len,
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

    return {result.subscription_id, std::move(arrow_schema)};
}

void SubscriberArrow::Unsubscribe(uint64_t subscription_id) {
    subscriber_->Unsubscribe(subscription_id);
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
