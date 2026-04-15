#include "fast_dds_capi.h"
#include "fast_dds_pubsub_provider.hpp"
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

extern "C" FletcherPubSub* fletcher_fastdds_new(
    uint32_t domain_id, uint32_t max_payload_bytes, char** out_error) {
    try {
        auto provider = std::make_shared<fletcher::FastDDSPubSubProvider>(
            domain_id, max_payload_bytes);
        return fletcher::MakeFletcherPubSub(std::move(provider));
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return nullptr;
    }
}
