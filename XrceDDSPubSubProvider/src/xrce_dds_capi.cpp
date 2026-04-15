#include "xrce_dds_capi.h"
#include "xrce_dds_pubsub_provider.hpp"
#include "pubsub/pubsub_capi_provider.hpp"

#include <cstring>
#include <memory>

static char* dup_error(const char* msg) {
    if (!msg) return nullptr;
    auto len = std::strlen(msg);
    auto* s  = static_cast<char*>(std::malloc(len + 1));
    if (s) std::memcpy(s, msg, len + 1);
    return s;
}

extern "C" FletcherXrceConfig fletcher_xrce_config_default(void) {
    FletcherXrceConfig c{};
    c.transport       = FLETCHER_XRCE_TRANSPORT_UDP;
    c.agent_ip        = "127.0.0.1";
    c.agent_port      = 2018;
    c.serial_device   = nullptr;
    c.serial_baudrate = 115200;
    c.max_payload     = 512;
    c.stream_history  = 4;
    c.run_loop_ms     = 10;
    c.session_key     = 0xAABBCCDD;
    return c;
}

extern "C" FletcherPubSub* fletcher_xrce_new(
    const FletcherXrceConfig* config, char** out_error) {
    try {
        fletcher::XrceConfig cfg;
        if (config) {
            switch (config->transport) {
                case FLETCHER_XRCE_TRANSPORT_TCP:
                    cfg.transport = fletcher::XrceTransport::kTcp; break;
                case FLETCHER_XRCE_TRANSPORT_SERIAL:
                    cfg.transport = fletcher::XrceTransport::kSerial; break;
                default:
                    cfg.transport = fletcher::XrceTransport::kUdp; break;
            }
            if (config->agent_ip)
                cfg.agent_ip = config->agent_ip;
            cfg.agent_port      = config->agent_port;
            if (config->serial_device)
                cfg.serial_device = config->serial_device;
            cfg.serial_baudrate = config->serial_baudrate;
            cfg.max_payload     = config->max_payload;
            cfg.stream_history  = config->stream_history;
            cfg.run_loop_ms     = config->run_loop_ms;
            cfg.session_key     = config->session_key;
        }
        auto provider =
            std::make_shared<fletcher::XrceDDSPubSubProvider>(cfg);
        return fletcher::MakeFletcherPubSub(std::move(provider));
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return nullptr;
    }
}
