#ifndef FLETCHER_PUBSUB_INCLUDE_PUBSUB_CAPI_PROVIDER_HPP_
#define FLETCHER_PUBSUB_INCLUDE_PUBSUB_CAPI_PROVIDER_HPP_

// C++ helper for provider libraries that need to create FletcherPubSub*
// handles for the C API.  Not part of the public C API itself.

#include "pubsub/pubsub.hpp"
#include "pubsub/pubsub_capi.h"

#include <memory>

namespace fletcher {

// Wrap a PubSub implementation in an opaque C handle.
// Called by provider-specific C factory functions.
FletcherPubSub* MakeFletcherPubSub(std::shared_ptr<PubSub> ptr);

}  // namespace fletcher

#endif  // FLETCHER_PUBSUB_INCLUDE_PUBSUB_CAPI_PROVIDER_HPP_
