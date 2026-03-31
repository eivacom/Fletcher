#ifndef ARROW_ROW_CODEC_INCLUDE_ARROW_ROW_CODEC_CAPI_H_
#define ARROW_ROW_CODEC_INCLUDE_ARROW_ROW_CODEC_CAPI_H_

// Pure-C public API for the ArrowRowCodec library.
//
// Schema and record-batch data are exchanged as Arrow IPC stream bytes:
//   - schema_ipc: an Arrow IPC stream written with zero record batches
//     (schema header only).
//   - batch_ipc:  an Arrow IPC stream containing exactly one record batch.
//
// All strings and byte buffers returned by this API must be freed with the
// corresponding arrow_row_free_* function.  Ownership is documented per
// function.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void ArrowRowCodec;

// Free a string returned by this API.
void arrow_row_free_string(char* s);

// Free a byte buffer returned by this API.
void arrow_row_free_bytes(uint8_t* data);

// ---------------------------------------------------------------------------
// RowCodec
// ---------------------------------------------------------------------------

// Create a RowCodec from a schema-only Arrow IPC stream.
// Returns NULL and sets *out_error (caller must free) on failure.
ArrowRowCodec* arrow_row_codec_new(const uint8_t* schema_ipc,
                                    size_t         schema_ipc_len,
                                    char**         out_error);

void arrow_row_codec_free(ArrowRowCodec* codec);

// Encode one row.
// ipc_data: Arrow IPC stream with exactly one record batch (one row).
// out_data/out_len: encoded row bytes, caller must free with
//   arrow_row_free_bytes.
bool arrow_row_codec_encode_row(const ArrowRowCodec* codec,
                                 const uint8_t*       ipc_data,
                                 size_t               ipc_len,
                                 uint8_t**            out_data,
                                 size_t*              out_len,
                                 char**               out_error);

// Decode one row.
// out_ipc/out_ipc_len: Arrow IPC stream with one record batch (one row),
//   caller must free with arrow_row_free_bytes.
bool arrow_row_codec_decode_row(const ArrowRowCodec* codec,
                                 const uint8_t*       row_data,
                                 size_t               row_len,
                                 uint8_t**            out_ipc,
                                 size_t*              out_ipc_len,
                                 char**               out_error);

#ifdef __cplusplus
}
#endif

#endif  // ARROW_ROW_CODEC_INCLUDE_ARROW_ROW_CODEC_CAPI_H_
