#ifndef ARROW_ROW_INCLUDE_ARROW_ROW_CAPI_H_
#define ARROW_ROW_INCLUDE_ARROW_ROW_CAPI_H_

// Pure-C public API for the arrow_row library.
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
typedef void ArrowRowSQLiteWAL;
typedef void ArrowRowBatcher;

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
// out_data/out_len: encoded ArrowRow bytes, caller must free with
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

// ---------------------------------------------------------------------------
// SQLiteWAL
// ---------------------------------------------------------------------------

// Open (or create) a SQLite write-ahead log at path.
// Pass ":memory:" for an in-memory database.
ArrowRowSQLiteWAL* arrow_row_sqlite_wal_new(const char* path, char** out_error);

void arrow_row_sqlite_wal_free(ArrowRowSQLiteWAL* wal);

// ---------------------------------------------------------------------------
// GenericRowBatcher
// ---------------------------------------------------------------------------

// Flush callback invoked on a background thread when a full batch is ready.
// ipc_data/ipc_len: Arrow IPC stream for the batch; valid only for the
//   duration of the callback — do not store the pointer.
// Return true to signal success, false to signal failure.
typedef bool (*ArrowRowFlushFn)(const uint8_t* ipc_data,
                                 size_t         ipc_len,
                                 void*          userdata);

// Create a GenericRowBatcher.
// schema_ipc: schema-only Arrow IPC stream matching the rows to be appended.
ArrowRowBatcher* arrow_row_batcher_new(ArrowRowSQLiteWAL* wal,
                                        const uint8_t*     schema_ipc,
                                        size_t             schema_ipc_len,
                                        int64_t            batch_size,
                                        ArrowRowFlushFn    on_flush,
                                        void*              userdata,
                                        char**             out_error);

// Destroy the batcher, waiting for all in-flight flush callbacks to complete.
void arrow_row_batcher_free(ArrowRowBatcher* batcher);

// Append one encoded row (produced by arrow_row_codec_encode_row).
// Thread-safe; returns false and sets *out_error on failure.
bool arrow_row_batcher_append(ArrowRowBatcher* batcher,
                               const uint8_t*   row_data,
                               size_t           row_len,
                               char**           out_error);

// Rows accumulated since the last automatic flush.
int64_t arrow_row_batcher_row_count(const ArrowRowBatcher* batcher);

#ifdef __cplusplus
}
#endif

#endif  // ARROW_ROW_INCLUDE_ARROW_ROW_CAPI_H_
