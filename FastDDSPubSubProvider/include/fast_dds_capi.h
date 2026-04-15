#ifndef FLETCHER_FAST_DDS_CAPI_H_
#define FLETCHER_FAST_DDS_CAPI_H_

// C factory function for the FastDDS PubSub provider.
// Returns a FletcherPubSub* handle for use with fletcher_driver_new().

#include "pubsub/pubsub_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create a FastDDS provider.
//   domain_id:         DDS domain ID (0 is the default).
//   max_payload_bytes: Maximum serialized envelope size (default: 1 MB).
// Returns NULL and sets *out_error on failure.
FletcherPubSub* fletcher_fastdds_new(uint32_t domain_id,
                                     uint32_t max_payload_bytes,
                                     char**   out_error);

#ifdef __cplusplus
}
#endif

#endif  // FLETCHER_FAST_DDS_CAPI_H_
