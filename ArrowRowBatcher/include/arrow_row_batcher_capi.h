#ifndef ARROW_ROW_BATCHER_INCLUDE_ARROW_ROW_BATCHER_CAPI_H_
#define ARROW_ROW_BATCHER_INCLUDE_ARROW_ROW_BATCHER_CAPI_H_

// Pure-C public API for the ArrowRowBatcher library.
//
// This header includes arrow_row_codec_capi.h and therefore also exposes
// arrow_row_free_string, arrow_row_free_bytes, and the RowCodec functions.
// Callers who use both libraries need only include this header.

#include "arrow_row_codec_capi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void ArrowRowSQLiteWAL;
typedef void ArrowRowBatcher;

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

#endif  // ARROW_ROW_BATCHER_INCLUDE_ARROW_ROW_BATCHER_CAPI_H_
