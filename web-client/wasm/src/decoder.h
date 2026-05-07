/*
 * Fletcher WASM Row Decoder
 *
 * Legacy decoder for the tagged row wire format.  The primary wire format
 * is now positional (see positional-encoder.ts / positional-decoder.ts).
 * This module is retained for backward compatibility with older publishers.
 *
 * Parses the tagged row wire format into a flat field table that
 * TypeScript can read directly from WASM linear memory via DataView.
 *
 * Field table entry layout (14 bytes each, little-endian):
 *   [FIELD_NUM   : 4 bytes] uint32_t
 *   [WIRE_TYPE   : 1 byte ] WireTypeId
 *   [NULL_FLAG   : 1 byte ] 0x00 = valid, 0x01 = null
 *   [DATA_OFFSET : 4 bytes] uint32_t — byte offset into original input
 *   [DATA_LEN    : 4 bytes] uint32_t — 0 when null
 */

#ifndef FLETCHER_WASM_DECODER_H_
#define FLETCHER_WASM_DECODER_H_

#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define FLETCHER_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define FLETCHER_EXPORT
#endif

#define FLETCHER_FIELD_ENTRY_SIZE 14

/* -------------------------------------------------------------------
 * fletcher_decode_row
 *
 * Parse a full tagged row (with 8-byte schema_hash + 2-byte field_count
 * header) and write field table entries to out_table.
 *
 * Returns:
 *   >= 0  number of fields written
 *   -1    parse error (buffer truncated, etc.)
 * ------------------------------------------------------------------- */
FLETCHER_EXPORT
int32_t fletcher_decode_row(
    const uint8_t* data, uint32_t len,
    uint8_t* out_table, uint32_t out_table_cap);

/* -------------------------------------------------------------------
 * fletcher_decode_struct
 *
 * Parse a struct payload (no schema_hash header — starts with
 * 2-byte field_count) and write field table entries.
 *
 * Returns same as fletcher_decode_row.
 * ------------------------------------------------------------------- */
FLETCHER_EXPORT
int32_t fletcher_decode_struct(
    const uint8_t* data, uint32_t len,
    uint8_t* out_table, uint32_t out_table_cap);

/* -------------------------------------------------------------------
 * fletcher_get_schema_hash
 *
 * Read the 8-byte schema hash from the beginning of a tagged row.
 * Writes the hash to *out_hash.  Returns 0 on success, -1 if
 * the buffer is too small.
 * ------------------------------------------------------------------- */
FLETCHER_EXPORT
int32_t fletcher_get_schema_hash(
    const uint8_t* data, uint32_t len,
    uint32_t* out_hash_lo, uint32_t* out_hash_hi);

/* -------------------------------------------------------------------
 * fletcher_list_count
 *
 * Read the 4-byte element count from the start of a list payload.
 * Returns the count, or 0 if the buffer is too small.
 * ------------------------------------------------------------------- */
FLETCHER_EXPORT
uint32_t fletcher_list_count(const uint8_t* data, uint32_t len);

/* -------------------------------------------------------------------
 * fletcher_map_count
 *
 * Read the 4-byte entry count from the start of a map payload.
 * Returns the count, or 0 if the buffer is too small.
 * ------------------------------------------------------------------- */
FLETCHER_EXPORT
uint32_t fletcher_map_count(const uint8_t* data, uint32_t len);

#endif /* FLETCHER_WASM_DECODER_H_ */
