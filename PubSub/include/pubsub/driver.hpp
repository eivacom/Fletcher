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

    /// Publish an ArrowRow with optional attachments to a topic.
    void Publish(const std::vector<std::string>& segments,
                 const ArrowRow& row,
                 const Attachments& attachments = {});

    /// Publish by writing encoded row directly into the provider's buffer.
    /// The encoder callback writes the row into the provided WriteBuffer.
    void PublishDirect(const std::vector<std::string>& segments,
                       PubSubProvider::RowEncoder encoder,
                       const Attachments& attachments = {});

    /// Subscribe to a topic.  Returns a subscription ID.
    /// Multiple subscribers per topic are supported; each receives
    /// every published message via internal fan-out.
    using SubscribeCallback = std::function<void(ArrowRow row,
                                                 Attachments attachments)>;
    uint64_t Subscribe(const std::vector<std::string>& segments,
                       SubscribeCallback cb);

    /// Remove a subscription by ID.
    void Unsubscribe(uint64_t subscription_id);

    /// Encode an ArrowRow using the codec for the given topic.
    /// Useful for relay scenarios (e.g. WebGateway).
    EncodedRow EncodeRow(const std::vector<std::string>& segments,
                         const ArrowRow& row) const;

    /// Decode an EncodedRow using the codec for the given topic.
    ArrowRow DecodeRow(const std::vector<std::string>& segments,
                       const EncodedRow& encoded) const;

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
