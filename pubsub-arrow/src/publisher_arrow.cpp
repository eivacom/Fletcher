// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub_arrow/publisher_arrow.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <stdexcept>

namespace fletcher {

namespace {

OwnedSchema ExportToNano(const arrow::Schema& schema) {
    OwnedSchema out;
    auto status = arrow::ExportSchema(schema, out.get());
    if (!status.ok()) {
        throw std::runtime_error("PublisherArrow: ExportSchema: " + status.ToString());
    }
    return out;
}

}  // anonymous namespace

PublisherArrow::PublisherArrow(std::shared_ptr<PubSubProvider> provider)
    : publisher_(std::make_unique<Publisher>(std::move(provider))) {}

PublisherArrow::~PublisherArrow() = default;

void PublisherArrow::CreateTopic(const std::vector<std::string>& segments,
                                 std::shared_ptr<arrow::Schema> schema) {
    OwnedSchema nano;
    if (schema) {
        nano = ExportToNano(*schema);
    }

    // Delegate to the underlying Publisher FIRST. If it throws (duplicate
    // topic, provider failure) we leave the local codec registry
    // untouched, so it stays in sync with the Publisher's view.
    publisher_->CreateTopic(segments, std::move(nano));

    if (schema) {
        std::string key = JoinSegments(segments);
        std::lock_guard lock(mu_);
        codecs_[key] = TopicCodec{schema, std::make_unique<Codec>(schema)};
    }
}

void PublisherArrow::Publish(const std::vector<std::string>& segments, const ArrowRow& row,
                             const Attachments& attachments) {
    std::string key = JoinSegments(segments);
    Codec* codec;
    {
        std::lock_guard lock(mu_);
        auto it = codecs_.find(key);
        if (it == codecs_.end()) {
            throw std::runtime_error("PublisherArrow::Publish: no codec for topic " + key);
        }
        codec = it->second.codec.get();
    }

    std::vector<uint8_t> encoded = codec->EncodeRow(row);
    publisher_->Publish(
        segments,
        [data = std::move(encoded)](WriteBuffer& buf) { buf.Append(data.data(), data.size()); },
        attachments);
}

void PublisherArrow::PublishDirect(const std::vector<std::string>& segments,
                                   PubSubProvider::RowEncoder encoder,
                                   const Attachments& attachments) {
    publisher_->Publish(segments, std::move(encoder), attachments);
}

std::vector<std::string> PublisherArrow::ListTopics() const { return publisher_->ListTopics(); }

std::string PublisherArrow::JoinSegments(const std::vector<std::string>& segs) {
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
