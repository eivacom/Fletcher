/**
 * Row encoder — pure TypeScript.
 *
 * Encodes a plain object (keyed by field name) into the tagged row
 * wire format.  This is the inverse of row-decoder + object-backend.
 */

import { WireTypeId, scalarByteSize } from '../wire-types.js';
import type { FieldDescriptor, SchemaDescriptor } from './schema-descriptor.js';

const textEncoder = new TextEncoder();

export function encodeRow(
  schema: SchemaDescriptor,
  values: Record<string, unknown>,
): Uint8Array {
  const parts: Uint8Array[] = [];

  // Schema hash (8 bytes, little-endian).
  const hashBuf = new Uint8Array(8);
  const hashView = new DataView(hashBuf.buffer);
  hashView.setUint32(0, Number(schema.schemaHash & 0xFFFFFFFFn), true);
  hashView.setUint32(4, Number((schema.schemaHash >> 32n) & 0xFFFFFFFFn), true);
  parts.push(hashBuf);

  // Field count (2 bytes).
  const countBuf = new Uint8Array(2);
  new DataView(countBuf.buffer).setUint16(0, schema.fields.length, true);
  parts.push(countBuf);

  // Fields.
  for (const fd of schema.fields) {
    const value = values[fd.name] ?? null;
    parts.push(encodeField(fd, value));
  }

  return concat(parts);
}

function encodeField(fd: FieldDescriptor, value: unknown): Uint8Array {
  const parts: Uint8Array[] = [];

  // Header: field_num (4) + wire_type (1) + null_flag (1)
  const header = new Uint8Array(6);
  const hView = new DataView(header.buffer);
  hView.setUint32(0, fd.fieldNumber, true);
  header[4] = fd.wireType;

  if (value === null || value === undefined) {
    header[5] = 1; // null
    return header;
  }

  header[5] = 0; // not null
  parts.push(header);

  // Encode payload, then prepend its length.
  const payload = encodePayload(fd, value);
  const lenBuf = new Uint8Array(4);
  new DataView(lenBuf.buffer).setUint32(0, payload.byteLength, true);
  parts.push(lenBuf);
  parts.push(payload);

  return concat(parts);
}

function encodePayload(fd: FieldDescriptor, value: unknown): Uint8Array {
  const wt = fd.wireType;

  // Composite types
  if (wt === WireTypeId.STRUCT) {
    return encodeStructPayload(fd, value as Record<string, unknown>);
  }
  if (wt === WireTypeId.LIST || wt === WireTypeId.LARGE_LIST) {
    return encodeListPayload(fd, value as unknown[]);
  }
  if (wt === WireTypeId.FIXED_SIZE_LIST) {
    return encodeFixedSizeListPayload(fd, value as unknown[]);
  }
  if (wt === WireTypeId.MAP) {
    return encodeMapPayload(fd, value as Map<unknown, unknown>);
  }

  // Scalar
  return encodeScalar(wt, value);
}

// -----------------------------------------------------------------------
// Scalar encoding
// -----------------------------------------------------------------------

function encodeScalar(wt: WireTypeId, value: unknown): Uint8Array {
  const fixed = scalarByteSize(wt);

  if (fixed > 0) {
    const buf = new Uint8Array(fixed);
    const view = new DataView(buf.buffer);

    switch (wt) {
      case WireTypeId.BOOL:
        buf[0] = value ? 1 : 0;
        break;
      case WireTypeId.INT8:
        view.setInt8(0, value as number);
        break;
      case WireTypeId.INT16:
        view.setInt16(0, value as number, true);
        break;
      case WireTypeId.INT32:
      case WireTypeId.DATE32:
      case WireTypeId.TIME32:
      case WireTypeId.INTERVAL_MONTHS:
        view.setInt32(0, value as number, true);
        break;
      case WireTypeId.INT64:
      case WireTypeId.DATE64:
      case WireTypeId.TIME64:
      case WireTypeId.TIMESTAMP_NANO:
      case WireTypeId.DURATION_NANO:
        view.setBigInt64(0, value as bigint, true);
        break;
      case WireTypeId.UINT8:
        buf[0] = value as number;
        break;
      case WireTypeId.UINT16:
      case WireTypeId.HALF_FLOAT:
        view.setUint16(0, value as number, true);
        break;
      case WireTypeId.UINT32:
        view.setUint32(0, value as number, true);
        break;
      case WireTypeId.UINT64:
        view.setBigUint64(0, value as bigint, true);
        break;
      case WireTypeId.FLOAT32:
        view.setFloat32(0, value as number, true);
        break;
      case WireTypeId.FLOAT64:
        view.setFloat64(0, value as number, true);
        break;
      case WireTypeId.INTERVAL_DAY_TIME: {
        const v = value as { days: number; milliseconds: number };
        view.setInt32(0, v.days, true);
        view.setInt32(4, v.milliseconds, true);
        break;
      }
      case WireTypeId.INTERVAL_MONTH_DAY_NANO: {
        const v = value as { months: number; days: number; nanoseconds: bigint };
        view.setInt32(0, v.months, true);
        view.setInt32(4, v.days, true);
        view.setBigInt64(8, v.nanoseconds, true);
        break;
      }
      case WireTypeId.DECIMAL128:
      case WireTypeId.DECIMAL256: {
        const src = value as Uint8Array;
        buf.set(src.subarray(0, fixed));
        break;
      }
    }
    return buf;
  }

  // Variable-length scalars: [LEN:4][DATA:LEN]
  switch (wt) {
    case WireTypeId.STRING:
    case WireTypeId.LARGE_STRING:
    case WireTypeId.STRING_VIEW: {
      const encoded = textEncoder.encode(value as string);
      const buf = new Uint8Array(4 + encoded.byteLength);
      new DataView(buf.buffer).setUint32(0, encoded.byteLength, true);
      buf.set(encoded, 4);
      return buf;
    }
    case WireTypeId.BINARY:
    case WireTypeId.LARGE_BINARY:
    case WireTypeId.BINARY_VIEW: {
      const src = value as Uint8Array;
      const buf = new Uint8Array(4 + src.byteLength);
      new DataView(buf.buffer).setUint32(0, src.byteLength, true);
      buf.set(src, 4);
      return buf;
    }
    case WireTypeId.FIXED_SIZE_BINARY: {
      const src = value as Uint8Array;
      return new Uint8Array(src);
    }
    default:
      throw new Error(`encodeScalar: unsupported wire type 0x${wt.toString(16)}`);
  }
}

// -----------------------------------------------------------------------
// Composite encoding
// -----------------------------------------------------------------------

function encodeStructPayload(fd: FieldDescriptor, value: Record<string, unknown>): Uint8Array {
  const fields = fd.fields ?? [];
  const parts: Uint8Array[] = [];

  // Field count.
  const countBuf = new Uint8Array(2);
  new DataView(countBuf.buffer).setUint16(0, fields.length, true);
  parts.push(countBuf);

  for (const subFd of fields) {
    const subVal = value[subFd.name] ?? null;
    parts.push(encodeField(subFd, subVal));
  }

  return concat(parts);
}

function encodeListPayload(fd: FieldDescriptor, values: unknown[]): Uint8Array {
  const elemDesc = fd.element;
  if (!elemDesc) throw new Error('List field missing element descriptor');

  const parts: Uint8Array[] = [];

  // Count.
  const countBuf = new Uint8Array(4);
  new DataView(countBuf.buffer).setUint32(0, values.length, true);
  parts.push(countBuf);

  for (const v of values) {
    if (v === null || v === undefined) {
      parts.push(new Uint8Array([1])); // null flag
    } else {
      parts.push(new Uint8Array([0])); // not null
      parts.push(encodeListElement(elemDesc, v));
    }
  }

  return concat(parts);
}

function encodeFixedSizeListPayload(fd: FieldDescriptor, values: unknown[]): Uint8Array {
  const elemDesc = fd.element;
  if (!elemDesc) throw new Error('FixedSizeList field missing element descriptor');

  const parts: Uint8Array[] = [];

  // No count prefix for fixed-size lists.
  for (const v of values) {
    if (v === null || v === undefined) {
      parts.push(new Uint8Array([1])); // null flag
    } else {
      parts.push(new Uint8Array([0])); // not null
      parts.push(encodeListElement(elemDesc, v));
    }
  }

  return concat(parts);
}

function encodeMapPayload(fd: FieldDescriptor, map: Map<unknown, unknown>): Uint8Array {
  const keyDesc = fd.mapKey;
  const valDesc = fd.mapValue;
  if (!keyDesc || !valDesc) throw new Error('Map field missing key/value descriptors');

  const parts: Uint8Array[] = [];

  // Count.
  const countBuf = new Uint8Array(4);
  new DataView(countBuf.buffer).setUint32(0, map.size, true);
  parts.push(countBuf);

  for (const [k, v] of map) {
    // Key (no null flag).
    parts.push(encodeListElement(keyDesc, k));

    // Value (with null flag).
    if (v === null || v === undefined) {
      parts.push(new Uint8Array([1]));
    } else {
      parts.push(new Uint8Array([0]));
      parts.push(encodeListElement(valDesc, v));
    }
  }

  return concat(parts);
}

function encodeListElement(elemDesc: FieldDescriptor, value: unknown): Uint8Array {
  const wt = elemDesc.wireType;

  if (wt === WireTypeId.STRUCT) {
    return encodeStructPayload(elemDesc, value as Record<string, unknown>);
  }

  // Scalar — raw encoding (no DATA_LEN wrapper in list context).
  return encodeScalar(wt, value);
}

// -----------------------------------------------------------------------
// Utility
// -----------------------------------------------------------------------

function concat(parts: Uint8Array[]): Uint8Array {
  let total = 0;
  for (const p of parts) total += p.byteLength;
  const result = new Uint8Array(total);
  let pos = 0;
  for (const p of parts) {
    result.set(p, pos);
    pos += p.byteLength;
  }
  return result;
}
