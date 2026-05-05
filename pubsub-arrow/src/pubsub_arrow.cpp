#include "pubsub_arrow/pubsub_arrow.hpp"

#include <pubsub/owned_schema.hpp>
#include <core/write_buffer.hpp>

#include <arrow/c/bridge.h>
#include <arrow/api.h>

#include <stdexcept>

namespace fletcher {

// -----------------------------------------------------------------------
// Arrow C Data Interface helpers
// -----------------------------------------------------------------------

namespace {

OwnedSchema ExportToNano(const arrow::Schema& schema) {
    OwnedSchema out;
    auto status = arrow::ExportSchema(schema, out.get());
    if (!status.ok())
        throw std::runtime_error("PubSubArrow: ExportSchema: " + status.ToString());
    return out;
}

std::shared_ptr<arrow::Schema> ImportFromNano(const ArrowSchema* schema) {
    if (!schema || !schema->release) return nullptr;
    OwnedSchema copy = OwnedSchema::DeepCopy(schema);
    auto result = arrow::ImportSchema(copy.get());
    if (!result.ok())
        throw std::runtime_error("PubSubArrow: ImportSchema: " +
                                 result.status().ToString());
    return *result;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

PubSubArrow::PubSubArrow(std::shared_ptr<PubSub> provider)
    : driver_(std::make_unique<Driver>(std::move(provider))) {}

// -----------------------------------------------------------------------
// Topic management
// -----------------------------------------------------------------------

void PubSubArrow::CreateTopic(const std::vector<std::string>& segments,
                               std::shared_ptr<arrow::Schema> schema,
                               std::any config) {
    OwnedSchema nano;
    if (schema) {
        nano = ExportToNano(*schema);
        std::string key = JoinSegments(segments);
        std::lock_guard lock(mu_);
        codecs_[key] = TopicCodec{schema,
                                   std::make_unique<Codec>(schema)};
    }
    driver_->CreateTopic(segments, std::move(nano), std::move(config));
}

// -----------------------------------------------------------------------
// Publish
// -----------------------------------------------------------------------

void PubSubArrow::Publish(const std::vector<std::string>& segments,
                           const ArrowRow& row,
                           const Attachments& attachments) {
    std::string key = JoinSegments(segments);
    Codec* codec;
    {
        std::lock_guard lock(mu_);
        auto it = codecs_.find(key);
        if (it == codecs_.end())
            throw std::runtime_error(
                "PubSubArrow::Publish: no codec for topic " + key);
        codec = it->second.codec.get();
    }

    auto encoded = codec->EncodeRow(row);
    driver_->Publish(segments,
        [data = std::move(encoded)](WriteBuffer& buf) {
            buf.Append(data.data(), data.size());
        },
        attachments);
}

void PubSubArrow::PublishDirect(const std::vector<std::string>& segments,
                                 PubSub::RowEncoder encoder,
                                 const Attachments& attachments) {
    driver_->Publish(segments, std::move(encoder), attachments);
}

// -----------------------------------------------------------------------
// Subscribe
// -----------------------------------------------------------------------

PubSubArrow::SubscribeResult PubSubArrow::Subscribe(
    const std::vector<std::string>& segments, SubscribeCallback callback,
    std::any config) {
    std::string key = JoinSegments(segments);

    auto result = driver_->Subscribe(segments,
        [this, key, cb = std::move(callback)](
            const uint8_t* data, size_t len,
            SharedSchema /*schema*/, Attachments att) {
            Codec* codec;
            {
                std::lock_guard lock(mu_);
                auto it = codecs_.find(key);
                if (it == codecs_.end()) return;
                codec = it->second.codec.get();
            }
            auto row = codec->DecodeRow(data, len);
            cb(std::move(row), std::move(att));
        }, std::move(config));

    // Convert the nanoarrow schema to Arrow C++.
    std::shared_ptr<arrow::Schema> arrow_schema;
    if (result.schema) {
        arrow_schema = ImportFromNano(result.schema.get());
        std::lock_guard lock(mu_);
        if (codecs_.find(key) == codecs_.end()) {
            codecs_[key] = TopicCodec{
                arrow_schema,
                std::make_unique<Codec>(arrow_schema)};
        }
    }

    return {result.subscription_id, std::move(arrow_schema)};
}

// -----------------------------------------------------------------------
// Subscription management / introspection
// -----------------------------------------------------------------------

void PubSubArrow::Unsubscribe(uint64_t subscription_id) {
    driver_->Unsubscribe(subscription_id);
}

std::vector<std::string> PubSubArrow::ListTopics() const {
    return driver_->ListTopics();
}

bool PubSubArrow::HasTopic(const std::vector<std::string>& segments) const {
    return driver_->HasTopic(segments);
}

// -----------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------

std::string PubSubArrow::JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) return {};
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // namespace fletcher
