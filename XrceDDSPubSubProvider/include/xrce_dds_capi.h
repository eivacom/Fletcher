#ifndef FLETCHER_XRCE_DDS_CAPI_H_
#define FLETCHER_XRCE_DDS_CAPI_H_

// C factory function for the XRCE-DDS PubSub provider.
// Returns a FletcherPubSub* handle for use with fletcher_driver_new().

#include "pubsub/pubsub_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FLETCHER_XRCE_TRANSPORT_UDP    = 0,
    FLETCHER_XRCE_TRANSPORT_TCP    = 1,
    FLETCHER_XRCE_TRANSPORT_SERIAL = 2
} FletcherXrceTransport;

typedef struct {
    FletcherXrceTransport transport;
    const char*           agent_ip;
    uint16_t              agent_port;
    const char*           serial_device;
    uint32_t              serial_baudrate;
    uint32_t              max_payload;
    uint16_t              stream_history;
    uint32_t              run_loop_ms;
    uint32_t              session_key;
} FletcherXrceConfig;

// Return a config struct initialized with default values.
FletcherXrceConfig fletcher_xrce_config_default(void);

// Create an XRCE-DDS provider.
// Returns NULL and sets *out_error on failure (e.g. agent not reachable).
FletcherPubSub* fletcher_xrce_new(const FletcherXrceConfig* config,
                                  char**                    out_error);

#ifdef __cplusplus
}
#endif

#endif  // FLETCHER_XRCE_DDS_CAPI_H_
