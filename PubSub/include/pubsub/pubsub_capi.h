#ifndef FLETCHER_PUBSUB_INCLUDE_PUBSUB_CAPI_H_
#define FLETCHER_PUBSUB_INCLUDE_PUBSUB_CAPI_H_

// Pure-C public API for the PubSub library (Driver, schema, attachments).
//
// Provider-specific factory functions live in their own headers
// (e.g. fast_dds_capi.h, xrce_dds_capi.h) and return FletcherPubSub*
// handles that can be passed to fletcher_driver_new().
//
// Schema data is exchanged via the Arrow C Data Interface (ArrowSchema
// struct) — no IPC serialization needed.
//
// All strings returned by this API must be freed with fletcher_free_string().
// Ownership is documented per function.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward-declare nanoarrow's ArrowSchema (Arrow C Data Interface).
struct ArrowSchema;

// ---------------------------------------------------------------------------
// Opaque handles
// ---------------------------------------------------------------------------

typedef void FletcherPubSub;       // Provider instance (shared ownership)
typedef void FletcherDriver;       // Driver instance
typedef void FletcherAttachments;  // Key-value sidecar data

// ---------------------------------------------------------------------------
// Memory management
// ---------------------------------------------------------------------------

// Free a string returned by this API (e.g. error messages).
void fletcher_free_string(char* s);

// Free a FletcherPubSub handle.  The underlying provider is destroyed when
// the last reference (including any Driver still using it) is released.
void fletcher_pubsub_free(FletcherPubSub* provider);

// ---------------------------------------------------------------------------
// Attachments — read-only accessors
//
// Used in subscribe callbacks.  Pointers are valid for the callback duration.
// ---------------------------------------------------------------------------

size_t         fletcher_attachments_count(const FletcherAttachments* a);
const char*    fletcher_attachments_key(const FletcherAttachments* a,
                                        size_t index);
const uint8_t* fletcher_attachments_value(const FletcherAttachments* a,
                                          size_t index,
                                          size_t* out_len);

// ---------------------------------------------------------------------------
// Attachments — builder (for publish)
// ---------------------------------------------------------------------------

FletcherAttachments* fletcher_attachments_new(void);
void fletcher_attachments_add(FletcherAttachments* a,
                              const char* key,
                              const uint8_t* data, size_t len);
void fletcher_attachments_free(FletcherAttachments* a);

// ---------------------------------------------------------------------------
// Subscribe callback
//
// Called from the provider's delivery thread.  |data| and |schema| are
// borrowed for the callback duration — deep-copy if needed beyond that.
// ---------------------------------------------------------------------------

typedef void (*FletcherSubscribeCallback)(
    const uint8_t*              data,
    size_t                      len,
    const struct ArrowSchema*   schema,
    const FletcherAttachments*  attachments,
    void*                       user_data);

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

// Create a Driver wrapping the given provider.  Takes shared ownership of
// the provider (caller may free their handle independently).
// Returns NULL and sets *out_error on failure.
FletcherDriver* fletcher_driver_new(FletcherPubSub* provider,
                                    char**          out_error);

void fletcher_driver_free(FletcherDriver* driver);

// Declare a topic with the given schema.  Deep-copies *schema internally;
// caller retains ownership of their ArrowSchema.
bool fletcher_driver_create_topic(FletcherDriver*           driver,
                                  const char* const*        segments,
                                  size_t                    segment_count,
                                  const struct ArrowSchema* schema,
                                  char**                    out_error);

// Publish pre-encoded row bytes.  |attachments| may be NULL.
bool fletcher_driver_publish(FletcherDriver*            driver,
                             const char* const*         segments,
                             size_t                     segment_count,
                             const uint8_t*             row_data,
                             size_t                     row_len,
                             const FletcherAttachments* attachments,
                             char**                     out_error);

// Subscribe to a topic.  On success, writes the subscription ID and
// deep-copies the topic schema into *out_schema (caller must release it
// via out_schema->release(out_schema) when done).
bool fletcher_driver_subscribe(FletcherDriver*            driver,
                               const char* const*         segments,
                               size_t                     segment_count,
                               FletcherSubscribeCallback  callback,
                               void*                      user_data,
                               uint64_t*                  out_subscription_id,
                               struct ArrowSchema*        out_schema,
                               char**                     out_error);

bool fletcher_driver_unsubscribe(FletcherDriver* driver,
                                 uint64_t        subscription_id,
                                 char**          out_error);

// Query whether a topic has been created.
bool fletcher_driver_has_topic(const FletcherDriver* driver,
                               const char* const*    segments,
                               size_t                segment_count);

#ifdef __cplusplus
}
#endif

#endif  // FLETCHER_PUBSUB_INCLUDE_PUBSUB_CAPI_H_
