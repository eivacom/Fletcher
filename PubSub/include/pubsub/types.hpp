#ifndef FLETCHER_INCLUDE_PUBSUB_TYPES_HPP_
#define FLETCHER_INCLUDE_PUBSUB_TYPES_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fletcher {

// Opaque binary blob.  shared_ptr gives zero-copy sharing from publisher
// through provider to subscriber callback.
using Blob = std::shared_ptr<const std::vector<uint8_t>>;

// Key-value sidecar data attached to a message during transit.
using Attachments = std::unordered_map<std::string, Blob>;

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_TYPES_HPP_
