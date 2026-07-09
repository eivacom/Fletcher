// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_PUBLISHER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PUBLISHER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/provider.hpp"

namespace fletcher {

/// High-level publisher API. Holds a topic registry and forwards
/// CreateTopic / Publish calls to the underlying provider.
///
/// Construct with a shared_ptr to any PubSubProvider implementation
/// (FastDDS, XRCE-DDS, in-process loopback, etc.). The Publisher does
/// not own the provider; it shares ownership so that a single provider
/// can back both a Publisher and a Subscriber in the same process.
///
/// Thread safety: all public methods are safe to call from any thread.
class Publisher {
   public:
    explicit Publisher(std::shared_ptr<PubSubProvider> provider);
    ~Publisher();

    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;

    /// Create a topic on the underlying provider and register it in the
    /// local topic registry. Throws if the topic already exists.
    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema);

    /// Publish by writing the encoded row directly into the provider's
    /// transport buffer.
    void Publish(const std::vector<std::string>& segments, PubSubProvider::RowEncoder encoder,
                 const Attachments& attachments = {});

    /// List all registered topic names (segments joined with "/").
    [[nodiscard]] std::vector<std::string> ListTopics() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PUBLISHER_HPP_
