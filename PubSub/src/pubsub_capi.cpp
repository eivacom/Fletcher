#include "pubsub/pubsub_capi.h"

#include "pubsub/driver.hpp"
#include "pubsub/owned_schema.hpp"
#include "pubsub/types.hpp"
#include "pubsub/write_buffer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Internal handle types
// ---------------------------------------------------------------------------

struct FletcherPubSubImpl {
    std::shared_ptr<PubSub> ptr;
};

struct FletcherDriverImpl {
    std::unique_ptr<Driver> driver;
};

// Tag to distinguish heap-allocated (builder) attachments from borrowed
// (callback) attachments.  Only builder instances may be freed/mutated.
struct FletcherAttachmentsBuilder {
    Attachments map;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static char* dup_error(const char* msg) {
    if (!msg) return nullptr;
    auto len = std::strlen(msg);
    auto* s  = static_cast<char*>(std::malloc(len + 1));
    if (s) std::memcpy(s, msg, len + 1);
    return s;
}

static char* dup_error(const std::string& msg) {
    return dup_error(msg.c_str());
}

static std::vector<std::string> to_segments(const char* const* segs,
                                            size_t count) {
    std::vector<std::string> v;
    v.reserve(count);
    for (size_t i = 0; i < count; ++i)
        v.emplace_back(segs[i]);
    return v;
}

// Interpret a FletcherAttachments* as an Attachments map.
// Works for both borrowed (callback) and builder instances.
static const Attachments& as_map(const FletcherAttachments* a) {
    return reinterpret_cast<const FletcherAttachmentsBuilder*>(a)->map;
}

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------

extern "C" void fletcher_free_string(char* s) { std::free(s); }

extern "C" void fletcher_pubsub_free(FletcherPubSub* provider) {
    delete reinterpret_cast<FletcherPubSubImpl*>(provider);
}

// ---------------------------------------------------------------------------
// Attachments — read-only accessors
// ---------------------------------------------------------------------------

extern "C" size_t fletcher_attachments_count(
    const FletcherAttachments* a) {
    if (!a) return 0;
    return as_map(a).size();
}

extern "C" const char* fletcher_attachments_key(
    const FletcherAttachments* a, size_t index) {
    const auto& m = as_map(a);
    auto it = m.begin();
    std::advance(it, index);
    return it->first.c_str();
}

extern "C" const uint8_t* fletcher_attachments_value(
    const FletcherAttachments* a, size_t index, size_t* out_len) {
    const auto& m = as_map(a);
    auto it = m.begin();
    std::advance(it, index);
    const auto& blob = it->second;
    if (blob && !blob->empty()) {
        if (out_len) *out_len = blob->size();
        return blob->data();
    }
    if (out_len) *out_len = 0;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Attachments — builder
// ---------------------------------------------------------------------------

extern "C" FletcherAttachments* fletcher_attachments_new(void) {
    return reinterpret_cast<FletcherAttachments*>(
        new FletcherAttachmentsBuilder());
}

extern "C" void fletcher_attachments_add(
    FletcherAttachments* a, const char* key,
    const uint8_t* data, size_t len) {
    auto* builder = reinterpret_cast<FletcherAttachmentsBuilder*>(a);
    auto blob = std::make_shared<const std::vector<uint8_t>>(data, data + len);
    builder->map[key] = std::move(blob);
}

extern "C" void fletcher_attachments_free(FletcherAttachments* a) {
    delete reinterpret_cast<FletcherAttachmentsBuilder*>(a);
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

extern "C" FletcherDriver* fletcher_driver_new(
    FletcherPubSub* provider, char** out_error) {
    if (!provider) {
        if (out_error) *out_error = dup_error("provider is null");
        return nullptr;
    }
    try {
        auto* pimpl = reinterpret_cast<FletcherPubSubImpl*>(provider);
        auto* d = new FletcherDriverImpl();
        d->driver = std::make_unique<Driver>(pimpl->ptr);
        return reinterpret_cast<FletcherDriver*>(d);
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return nullptr;
    }
}

extern "C" void fletcher_driver_free(FletcherDriver* driver) {
    delete reinterpret_cast<FletcherDriverImpl*>(driver);
}

extern "C" bool fletcher_driver_create_topic(
    FletcherDriver* driver,
    const char* const* segments, size_t segment_count,
    const struct ArrowSchema* schema,
    char** out_error) {
    try {
        auto* d = reinterpret_cast<FletcherDriverImpl*>(driver);
        auto owned = OwnedSchema::DeepCopy(schema);
        d->driver->CreateTopic(to_segments(segments, segment_count),
                               std::move(owned));
        return true;
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return false;
    }
}

extern "C" bool fletcher_driver_publish(
    FletcherDriver* driver,
    const char* const* segments, size_t segment_count,
    const uint8_t* row_data, size_t row_len,
    const FletcherAttachments* attachments,
    char** out_error) {
    try {
        auto* d = reinterpret_cast<FletcherDriverImpl*>(driver);
        auto segs = to_segments(segments, segment_count);

        // Wrap pre-encoded bytes in a RowEncoder that appends them to the
        // provider's WriteBuffer — a single memcpy.
        auto encoder = [row_data, row_len](WriteBuffer& buf) {
            buf.Append(row_data, row_len);
        };

        if (attachments) {
            d->driver->Publish(segs, encoder, as_map(attachments));
        } else {
            d->driver->Publish(segs, encoder);
        }
        return true;
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return false;
    }
}

extern "C" bool fletcher_driver_subscribe(
    FletcherDriver* driver,
    const char* const* segments, size_t segment_count,
    FletcherSubscribeCallback callback, void* user_data,
    uint64_t* out_subscription_id,
    struct ArrowSchema* out_schema,
    char** out_error) {
    try {
        auto* d = reinterpret_cast<FletcherDriverImpl*>(driver);

        // Wrap the C callback + user_data in a std::function that adapts
        // the C++ types to C types.  SharedSchema → const ArrowSchema*,
        // Attachments → FletcherAttachments* (borrowed pointer cast).
        auto wrapper = [callback, user_data](
            const uint8_t* data, size_t len,
            SharedSchema schema, Attachments attachments) {
            // The Attachments local is alive for the callback duration.
            // Wrap it as a FletcherAttachmentsBuilder so the read accessors
            // can cast it back uniformly.
            FletcherAttachmentsBuilder view{std::move(attachments)};
            callback(data, len, schema.get(),
                     reinterpret_cast<const FletcherAttachments*>(&view),
                     user_data);
        };

        auto result = d->driver->Subscribe(
            to_segments(segments, segment_count), wrapper);

        if (out_subscription_id)
            *out_subscription_id = result.subscription_id;

        // Export the schema via the Arrow C Data Interface.
        if (out_schema && result.schema) {
            auto copy = OwnedSchema::DeepCopy(result.schema.get());
            // Move into the caller's ArrowSchema by copying the struct
            // and clearing our release callback so ~OwnedSchema won't
            // double-free.
            std::memcpy(out_schema, copy.get(), sizeof(ArrowSchema));
            copy.get()->release = nullptr;
        }
        return true;
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return false;
    }
}

extern "C" bool fletcher_driver_unsubscribe(
    FletcherDriver* driver, uint64_t subscription_id,
    char** out_error) {
    try {
        auto* d = reinterpret_cast<FletcherDriverImpl*>(driver);
        d->driver->Unsubscribe(subscription_id);
        return true;
    } catch (const std::exception& e) {
        if (out_error) *out_error = dup_error(e.what());
        return false;
    }
}

extern "C" bool fletcher_driver_has_topic(
    const FletcherDriver* driver,
    const char* const* segments, size_t segment_count) {
    auto* d = reinterpret_cast<const FletcherDriverImpl*>(driver);
    return d->driver->HasTopic(to_segments(segments, segment_count));
}

// ---------------------------------------------------------------------------
// Provider handle factory — used by provider-specific C APIs.
//
// Not part of the public C API; exposed via a C++ header for provider
// libraries to call.
// ---------------------------------------------------------------------------

namespace fletcher {
FletcherPubSub* MakeFletcherPubSub(std::shared_ptr<PubSub> ptr) {
    auto* impl = new FletcherPubSubImpl{std::move(ptr)};
    return reinterpret_cast<FletcherPubSub*>(impl);
}
}  // namespace fletcher
