#include "pubsub/driver.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace fletcher {

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

namespace {

std::string JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) return {};
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// Impl
// -----------------------------------------------------------------------

struct Driver::Impl {
    struct TopicState {
        std::vector<std::string>       segments;
        std::shared_ptr<arrow::Schema> schema;
        std::unique_ptr<RowCodec>      codec;
        bool                           provider_subscribed = false;
    };

    struct Subscription {
        std::string      topic_key;
        SubscribeCallback callback;
    };

    std::shared_ptr<PubSubProvider> provider;

    mutable std::mutex mu;
    std::unordered_map<std::string, TopicState>    topics;       // key = joined name
    std::unordered_map<uint64_t, Subscription>     subscriptions;
    std::atomic<uint64_t> next_id{1};

    // Ensure the provider has a single subscription for this topic that
    // fans out to all registered Driver subscriptions.
    void EnsureProviderSubscription(const std::string& key, TopicState& ts) {
        if (ts.provider_subscribed) return;

        provider->Subscribe(ts.segments,
            [this, key](ArrowRow row, Attachments att) {
                std::vector<SubscribeCallback> cbs;
                {
                    std::lock_guard lock(mu);
                    for (auto& [id, sub] : subscriptions) {
                        if (sub.topic_key == key)
                            cbs.push_back(sub.callback);
                    }
                }
                for (auto& cb : cbs)
                    cb(row, att);
            });

        ts.provider_subscribed = true;
    }
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

Driver::Driver(std::shared_ptr<PubSubProvider> provider)
    : impl_(std::make_unique<Impl>()) {
    if (!provider)
        throw std::invalid_argument("Driver: provider must not be null");
    impl_->provider = std::move(provider);
}

Driver::~Driver() {
    std::lock_guard lock(impl_->mu);
    for (auto& [key, ts] : impl_->topics) {
        if (ts.provider_subscribed) {
            impl_->provider->Unsubscribe(ts.segments);
            ts.provider_subscribed = false;
        }
    }
}

// -----------------------------------------------------------------------
// Topic management
// -----------------------------------------------------------------------

void Driver::CreateTopic(const std::vector<std::string>& segments,
                         std::shared_ptr<arrow::Schema> schema) {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);

    if (impl_->topics.count(key))
        throw std::runtime_error("Driver: topic already exists: " + key);

    impl_->provider->CreateTopic(segments, schema);
    impl_->provider->RegisterCodec(key, schema);

    Impl::TopicState ts;
    ts.segments = segments;
    ts.schema   = schema;
    if (schema)
        ts.codec = std::make_unique<RowCodec>(schema);
    impl_->topics[key] = std::move(ts);
}

void Driver::Publish(const std::vector<std::string>& segments,
                     const ArrowRow& row,
                     const Attachments& attachments) {
    impl_->provider->Publish(segments, row, attachments);
}

void Driver::PublishDirect(const std::vector<std::string>& segments,
                           PubSubProvider::RowEncoder encoder,
                           const Attachments& attachments) {
    impl_->provider->PublishDirect(segments, std::move(encoder), attachments);
}

// -----------------------------------------------------------------------
// Subscription
// -----------------------------------------------------------------------

uint64_t Driver::Subscribe(const std::vector<std::string>& segments,
                           SubscribeCallback cb) {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(key);
    if (it == impl_->topics.end())
        throw std::runtime_error("Driver: unknown topic: " + key);

    uint64_t id = impl_->next_id.fetch_add(1);
    impl_->subscriptions[id] = Impl::Subscription{key, std::move(cb)};
    impl_->EnsureProviderSubscription(key, it->second);
    return id;
}

void Driver::Unsubscribe(uint64_t subscription_id) {
    std::lock_guard lock(impl_->mu);

    auto it = impl_->subscriptions.find(subscription_id);
    if (it == impl_->subscriptions.end())
        throw std::runtime_error("Driver: unknown subscription ID");

    std::string key = it->second.topic_key;
    impl_->subscriptions.erase(it);

    bool any_remaining = false;
    for (auto& [id, sub] : impl_->subscriptions) {
        if (sub.topic_key == key) {
            any_remaining = true;
            break;
        }
    }

    if (!any_remaining) {
        auto topic_it = impl_->topics.find(key);
        if (topic_it != impl_->topics.end() && topic_it->second.provider_subscribed) {
            impl_->provider->Unsubscribe(topic_it->second.segments);
            topic_it->second.provider_subscribed = false;
        }
    }
}

// -----------------------------------------------------------------------
// Codec helpers (for relay scenarios)
// -----------------------------------------------------------------------

EncodedRow Driver::EncodeRow(const std::vector<std::string>& segments,
                             const ArrowRow& row) const {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(key);
    if (it == impl_->topics.end())
        throw std::runtime_error("Driver: unknown topic: " + key);
    if (!it->second.codec)
        throw std::runtime_error("Driver: topic has no schema (cannot encode): " + key);

    return it->second.codec->EncodeRow(row);
}

ArrowRow Driver::DecodeRow(const std::vector<std::string>& segments,
                           const EncodedRow& encoded) const {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(key);
    if (it == impl_->topics.end())
        throw std::runtime_error("Driver: unknown topic: " + key);
    if (!it->second.codec)
        throw std::runtime_error("Driver: topic has no schema (cannot decode): " + key);

    return it->second.codec->DecodeRow(encoded);
}

// -----------------------------------------------------------------------
// Introspection
// -----------------------------------------------------------------------

std::vector<std::string> Driver::ListTopics() const {
    std::lock_guard lock(impl_->mu);
    std::vector<std::string> result;
    result.reserve(impl_->topics.size());
    for (auto& [key, _] : impl_->topics)
        result.push_back(key);
    return result;
}

bool Driver::HasTopic(const std::vector<std::string>& segments) const {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);
    return impl_->topics.count(key) > 0;
}

}  // namespace fletcher
