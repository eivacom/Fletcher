#ifndef FLETCHER_INCLUDE_PUBSUB_DRIVER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_DRIVER_HPP_

#include "pubsub/pubsub_provider.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fletcher {

/// High-level pub/sub API that wraps a PubSubProvider.
///
/// Unlike the raw provider (which allows one callback per topic), the
/// Driver supports multiple subscribers per topic via internal fan-out.
/// Each Subscribe() call returns a unique subscription ID for targeted
/// unsubscribe.  The Driver also maintains a topic registry for
/// introspection (ListTopics, HasTopic).
///
/// Thread safety: all public methods are safe to call from any thread.
class Driver {
 public:
    explicit Driver(std::shared_ptr<PubSubProvider> provider);
    ~Driver();

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    /// Create a topic on the underlying provider and register it
    /// in the local topic registry.
    void CreateTopic(const std::vector<std::string>& segments,
                     std::shared_ptr<arrow::Schema> schema);

    /// Publish an envelope to a topic.
    void Publish(const std::vector<std::string>& segments,
                 const Envelope& envelope);

    /// Result returned by Subscribe, carrying the subscription ID and
    /// the schema that the publisher provided when it created the topic.
    struct SubscribeResult {
        uint64_t subscription_id;
        std::shared_ptr<arrow::Schema> schema;
    };

    /// Subscribe to a topic.  Returns the subscription ID and the
    /// schema.  If the topic was not previously registered locally
    /// (e.g. a subscriber-only process), the provider is queried for
    /// the schema via the companion topic.
    using SubscribeCallback = std::function<void(const Envelope&)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments,
                              SubscribeCallback cb);

    /// Remove a subscription by ID.
    void Unsubscribe(uint64_t subscription_id);

    /// List all registered topic names (joined with "/").
    std::vector<std::string> ListTopics() const;

    /// Check whether a topic has been registered.
    bool HasTopic(const std::vector<std::string>& segments) const;

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_DRIVER_HPP_
