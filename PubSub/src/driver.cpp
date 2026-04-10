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
    // fans out to all registered Driver subscriptions.  Returns the
    // schema from the provider (only meaningful on the first call).
    std::shared_ptr<arrow::Schema> EnsureProviderSubscription(
        const std::string& key, TopicState& ts) {
        if (ts.provider_subscribed) return ts.schema;

        // Capture key by value; Impl* is safe because provider subscriptions
        // are torn down in the destructor before Impl is destroyed.
        auto result = provider->Subscribe(
            ts.segments, [this, key](const Envelope& env) {
                // Take a snapshot of callbacks under the lock, then invoke outside.
                std::vector<SubscribeCallback> cbs;
                {
                    std::lock_guard lock(mu);
                    for (auto& [id, sub] : subscriptions) {
                        if (sub.topic_key == key)
                            cbs.push_back(sub.callback);
                    }
                }
                for (auto& cb : cbs)
                    cb(env);
            });

        ts.provider_subscribed = true;
        if (result.schema && !ts.schema)
            ts.schema = result.schema;
        return ts.schema;
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
    // Unsubscribe from the provider for all topics to ensure callbacks
    // stop before Impl is destroyed.
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

    Impl::TopicState ts;
    ts.segments = segments;
    ts.schema   = std::move(schema);
    impl_->topics[key] = std::move(ts);
}

void Driver::Publish(const std::vector<std::string>& segments,
                     const Envelope& envelope) {
    impl_->provider->Publish(segments, envelope);
}

// -----------------------------------------------------------------------
// Subscription
// -----------------------------------------------------------------------

Driver::SubscribeResult Driver::Subscribe(
    const std::vector<std::string>& segments, SubscribeCallback cb) {
    std::string key = JoinSegments(segments);
    std::lock_guard lock(impl_->mu);

    // Auto-register the topic if it wasn't created locally (subscriber-only
    // process).  The provider's Subscribe will retrieve the schema.
    auto it = impl_->topics.find(key);
    if (it == impl_->topics.end()) {
        Impl::TopicState ts;
        ts.segments = segments;
        impl_->topics[key] = std::move(ts);
        it = impl_->topics.find(key);
    }

    uint64_t id = impl_->next_id.fetch_add(1);
    impl_->subscriptions[id] = Impl::Subscription{key, std::move(cb)};
    auto schema = impl_->EnsureProviderSubscription(key, it->second);
    return {id, schema};
}

void Driver::Unsubscribe(uint64_t subscription_id) {
    std::lock_guard lock(impl_->mu);

    auto it = impl_->subscriptions.find(subscription_id);
    if (it == impl_->subscriptions.end())
        throw std::runtime_error("Driver: unknown subscription ID");

    std::string key = it->second.topic_key;
    impl_->subscriptions.erase(it);

    // If no subscriptions remain for this topic, unsubscribe from provider.
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
