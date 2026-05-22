// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_DRIVER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_DRIVER_HPP_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/pubsub.hpp"

namespace fletcher {

/// High-level pub/sub API that wraps a PubSub.
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
    explicit Driver(std::shared_ptr<PubSub> provider);
    ~Driver();

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    /// Create a topic on the underlying provider and register it
    /// in the local topic registry.
    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema,
                     std::any config = {});

    /// Publish by writing encoded row directly into the provider's buffer.
    void Publish(const std::vector<std::string>& segments, PubSub::RowEncoder encoder,
                 const Attachments& attachments = {});

    /// Result returned by Subscribe.
    struct SubscribeResult {
        uint64_t subscription_id;
        OwnedSchema schema;
    };

    /// Subscribe to a topic.  Returns the subscription ID and the schema.
    using SubscribeCallback = std::function<void(const uint8_t* data, size_t len,
                                                 SharedSchema schema, Attachments attachments)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments, SubscribeCallback cb,
                              std::any config = {});

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
