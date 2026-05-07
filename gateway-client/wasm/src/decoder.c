/*
 * Fletcher WASM Row Decoder
 *
 * Parses the tagged row wire format into a flat field table.
 * See decoder.h for the field table entry layout.
 *
 * Wire format (little-endian):
 *   Row:    [SCHEMA_HASH:8] [FIELD_COUNT:2] {field}*
 *   Struct: [FIELD_COUNT:2] {field}*
 *   Field:  [FIELD_NUM:4] [WIRE_TYPE:1] [NULL_FLAG:1]
 *           if not null: [DATA_LEN:4] [PAYLOAD:DATA_LEN]
 */

#include "decoder.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian readers                                               */
/* ------------------------------------------------------------------ */

static uint16_t read_u16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t read_u32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ------------------------------------------------------------------ */
/* Little-endian writers (for field table entries)                      */
/* ------------------------------------------------------------------ */

static void write_u32(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* ------------------------------------------------------------------ */
/* Write one field table entry (14 bytes)                              */
/* ------------------------------------------------------------------ */

static int write_entry(uint8_t* out, uint32_t cap, int idx,
                       uint32_t field_num, uint8_t wire_type,
                       uint8_t null_flag, uint32_t data_offset,
                       uint32_t data_len) {
    uint32_t offset = (uint32_t)idx * FLETCHER_FIELD_ENTRY_SIZE;
    if (offset + FLETCHER_FIELD_ENTRY_SIZE > cap)
        return -1;

    uint8_t* e = out + offset;
    write_u32(e + 0, field_num);
    e[4] = wire_type;
    e[5] = null_flag;
    write_u32(e + 6, data_offset);
    write_u32(e + 10, data_len);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Core field-sequence parser                                          */
/*                                                                     */
/* Parses field_count tagged fields starting at data[pos].             */
/* base_offset is added to data_offset in each entry so that offsets   */
/* are relative to the original row buffer (not the struct payload).   */
/* ------------------------------------------------------------------ */

static int32_t parse_fields(const uint8_t* data, uint32_t len,
                            uint32_t pos, uint16_t field_count,
                            uint32_t base_offset,
                            uint8_t* out_table, uint32_t out_table_cap) {
    int idx = 0;

    for (uint16_t i = 0; i < field_count; i++) {
        /* field_num (4) + wire_type (1) + null_flag (1) = 6 bytes header */
        if (pos + 6 > len)
            return -1;

        uint32_t field_num = read_u32(data + pos);
        uint8_t  wire_type = data[pos + 4];
        uint8_t  null_flag = data[pos + 5];
        pos += 6;

        if (null_flag) {
            /* Null field — no data_len or payload. */
            if (write_entry(out_table, out_table_cap, idx,
                            field_num, wire_type, 1, 0, 0) < 0)
                return -1;
            idx++;
        } else {
            /* Valid field — read data_len then skip payload. */
            if (pos + 4 > len)
                return -1;

            uint32_t data_len = read_u32(data + pos);
            pos += 4;

            if (pos + data_len > len)
                return -1;

            uint32_t data_offset = base_offset + pos;

            if (write_entry(out_table, out_table_cap, idx,
                            field_num, wire_type, 0,
                            data_offset, data_len) < 0)
                return -1;
            idx++;

            pos += data_len;
        }
    }

    return (int32_t)idx;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

FLETCHER_EXPORT
int32_t fletcher_decode_row(const uint8_t* data, uint32_t len,
                            uint8_t* out_table, uint32_t out_table_cap) {
    /* Header: schema_hash (8) + field_count (2) = 10 bytes minimum. */
    if (len < 10)
        return -1;

    uint16_t field_count = read_u16(data + 8);
    return parse_fields(data, len, 10, field_count, 0,
                        out_table, out_table_cap);
}

FLETCHER_EXPORT
int32_t fletcher_decode_struct(const uint8_t* data, uint32_t len,
                               uint8_t* out_table, uint32_t out_table_cap) {
    /* Struct payload: field_count (2) then fields.  No schema_hash. */
    if (len < 2)
        return -1;

    uint16_t field_count = read_u16(data);
    return parse_fields(data, len, 2, field_count, 0,
                        out_table, out_table_cap);
}

FLETCHER_EXPORT
int32_t fletcher_get_schema_hash(const uint8_t* data, uint32_t len,
                                  uint32_t* out_hash_lo,
                                  uint32_t* out_hash_hi) {
    if (len < 8)
        return -1;

    /* Schema hash is stored as a uint64 little-endian.
     * We split it into two uint32s for easy consumption from JS
     * (avoids 64-bit integer issues in older environments). */
    *out_hash_lo = read_u32(data);
    *out_hash_hi = read_u32(data + 4);
    return 0;
}

FLETCHER_EXPORT
uint32_t fletcher_list_count(const uint8_t* data, uint32_t len) {
    if (len < 4)
        return 0;
    return read_u32(data);
}

FLETCHER_EXPORT
uint32_t fletcher_map_count(const uint8_t* data, uint32_t len) {
    if (len < 4)
        return 0;
    return read_u32(data);
}
