/**
 * Shared row decode orchestration.
 *
 * Uses the WasmDecoder to parse the field table, then dispatches
 * to a DecoderBackend to produce the final value.
 */

import { WireTypeId, scalarByteSize } from 'eiva-fletcher-codec';
import type { FieldDescriptor, SchemaDescriptor } from 'eiva-fletcher-codec';
import type { FieldEntry, WasmDecoder } from '../wasm-decoder.js';

const textDecoder = new TextDecoder();

/** Backend interface — implemented by object-backend and arrow-backend. */
export interface DecoderBackend<T> {
  decode(schema: SchemaDescriptor, row: Uint8Array, decoder: WasmDecoder): T;
}

// -----------------------------------------------------------------------
// Scalar readers — read a value from raw row bytes at a given offset.
// -----------------------------------------------------------------------

export function readScalar(
  data: Uint8Array,
  offset: number,
  length: number,
  wireType: WireTypeId,
): unknown {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);

  switch (wireType) {
    case WireTypeId.BOOL:
      return data[offset] !== 0;
    case WireTypeId.INT8:
      return view.getInt8(offset);
    case WireTypeId.INT16:
      return view.getInt16(offset, true);
    case WireTypeId.INT32:
      return view.getInt32(offset, true);
    case WireTypeId.INT64:
      return view.getBigInt64(offset, true);
    case WireTypeId.UINT8:
      return data[offset];
    case WireTypeId.UINT16:
      return view.getUint16(offset, true);
    case WireTypeId.UINT32:
      return view.getUint32(offset, true);
    case WireTypeId.UINT64:
      return view.getBigUint64(offset, true);
    case WireTypeId.FLOAT32:
      return view.getFloat32(offset, true);
    case WireTypeId.FLOAT64:
      return view.getFloat64(offset, true);
    case WireTypeId.DATE32:
      return view.getInt32(offset, true);
    case WireTypeId.DATE64:
      return view.getBigInt64(offset, true);
    case WireTypeId.TIMESTAMP_NANO:
      return view.getBigInt64(offset, true);
    case WireTypeId.TIME32:
      return view.getInt32(offset, true);
    case WireTypeId.TIME64:
      return view.getBigInt64(offset, true);
    case WireTypeId.DURATION_NANO:
      return view.getBigInt64(offset, true);
    case WireTypeId.HALF_FLOAT:
      return view.getUint16(offset, true);
    case WireTypeId.INTERVAL_MONTHS:
      return view.getInt32(offset, true);
    case WireTypeId.INTERVAL_DAY_TIME: {
      const days = view.getInt32(offset, true);
      const ms   = view.getInt32(offset + 4, true);
      return { days, milliseconds: ms };
    }
    case WireTypeId.INTERVAL_MONTH_DAY_NANO: {
      const months = view.getInt32(offset, true);
      const days   = view.getInt32(offset + 4, true);
      const nanos  = view.getBigInt64(offset + 8, true);
      return { months, days, nanoseconds: nanos };
    }
    case WireTypeId.DECIMAL128:
    case WireTypeId.DECIMAL256:
      return data.slice(offset, offset + length);
    case WireTypeId.STRING:
    case WireTypeId.LARGE_STRING:
    case WireTypeId.STRING_VIEW: {
      // Variable-length: [LEN:4][DATA:LEN]
      const strLen = view.getUint32(offset, true);
      return textDecoder.decode(data.subarray(offset + 4, offset + 4 + strLen));
    }
    case WireTypeId.BINARY:
    case WireTypeId.LARGE_BINARY:
    case WireTypeId.BINARY_VIEW:
    case WireTypeId.FIXED_SIZE_BINARY: {
      if (wireType === WireTypeId.FIXED_SIZE_BINARY) {
        return data.slice(offset, offset + length);
      }
      const binLen = view.getUint32(offset, true);
      return data.slice(offset + 4, offset + 4 + binLen);
    }
    default:
      throw new Error(`readScalar: unsupported wire type 0x${wireType.toString(16)}`);
  }
}

// -----------------------------------------------------------------------
// Composite readers
// -----------------------------------------------------------------------

/**
 * Decode a single field value (scalar or composite) from the raw row bytes.
 */
export function decodeFieldValue(
  data: Uint8Array,
  entry: FieldEntry,
  descriptor: FieldDescriptor,
  decoder: WasmDecoder,
): unknown {
  if (entry.isNull) return null;

  const wt = entry.wireType as WireTypeId;

  // Composite types
  if (wt === WireTypeId.STRUCT) {
    return decodeStruct(data, entry.dataOffset, entry.dataLength, descriptor, decoder);
  }
  if (wt === WireTypeId.LIST || wt === WireTypeId.LARGE_LIST) {
    return decodeList(data, entry.dataOffset, entry.dataLength, descriptor, decoder);
  }
  if (wt === WireTypeId.FIXED_SIZE_LIST) {
    return decodeFixedSizeList(data, entry.dataOffset, entry.dataLength, descriptor, decoder);
  }
  if (wt === WireTypeId.MAP) {
    return decodeMap(data, entry.dataOffset, entry.dataLength, descriptor, decoder);
  }

  // Scalar
  return readScalar(data, entry.dataOffset, entry.dataLength, wt);
}

function decodeStruct(
  data: Uint8Array,
  offset: number,
  length: number,
  descriptor: FieldDescriptor,
  decoder: WasmDecoder,
): Record<string, unknown> {
  const structData = data.subarray(offset, offset + length);
  const entries = decoder.decodeStruct(structData);
  const fields = descriptor.fields ?? [];

  const result: Record<string, unknown> = {};
  for (const fd of fields) {
    const entry = entries.find(e => e.fieldNumber === fd.fieldNumber);
    if (!entry) {
      result[fd.name] = null;
    } else {
      // Entry offsets are relative to structData, but we need absolute offsets
      // into the original data for readScalar. Adjust by adding the struct offset.
      const adjusted: FieldEntry = {
        ...entry,
        dataOffset: entry.dataOffset + offset,
      };
      result[fd.name] = decodeFieldValue(data, adjusted, fd, decoder);
    }
  }
  return result;
}

function decodeList(
  data: Uint8Array,
  offset: number,
  length: number,
  descriptor: FieldDescriptor,
  decoder: WasmDecoder,
): unknown[] {
  const elemDesc = descriptor.element;
  if (!elemDesc) throw new Error('List field missing element descriptor');

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  if (length < 4) throw new Error('List payload too short');

  const count = view.getUint32(offset, true);
  let pos = offset + 4;
  const result: unknown[] = new Array(count);

  for (let i = 0; i < count; i++) {
    if (pos >= offset + length) throw new Error('List: unexpected end');
    const nullFlag = data[pos];
    pos += 1;

    if (nullFlag) {
      result[i] = null;
    } else {
      result[i] = readListElement(data, pos, elemDesc, decoder);
      pos += elementByteSize(data, pos, elemDesc, decoder);
    }
  }
  return result;
}

function decodeFixedSizeList(
  data: Uint8Array,
  offset: number,
  length: number,
  descriptor: FieldDescriptor,
  decoder: WasmDecoder,
): unknown[] {
  const elemDesc = descriptor.element;
  if (!elemDesc) throw new Error('FixedSizeList field missing element descriptor');
  const fixedCount = descriptor.fixedSize ?? 0;

  let pos = offset;
  const result: unknown[] = new Array(fixedCount);

  for (let i = 0; i < fixedCount; i++) {
    if (pos >= offset + length) throw new Error('FixedSizeList: unexpected end');
    const nullFlag = data[pos];
    pos += 1;

    if (nullFlag) {
      result[i] = null;
    } else {
      result[i] = readListElement(data, pos, elemDesc, decoder);
      pos += elementByteSize(data, pos, elemDesc, decoder);
    }
  }
  return result;
}

function decodeMap(
  data: Uint8Array,
  offset: number,
  length: number,
  descriptor: FieldDescriptor,
  decoder: WasmDecoder,
): Map<unknown, unknown> {
  const keyDesc = descriptor.mapKey;
  const valDesc = descriptor.mapValue;
  if (!keyDesc || !valDesc) throw new Error('Map field missing key/value descriptors');

  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  if (length < 4) throw new Error('Map payload too short');

  const count = view.getUint32(offset, true);
  let pos = offset + 4;
  const result = new Map<unknown, unknown>();

  for (let i = 0; i < count; i++) {
    // Key (no null flag — keys are never null)
    const key = readListElement(data, pos, keyDesc, decoder);
    pos += elementByteSize(data, pos, keyDesc, decoder);

    // Value (has null flag)
    if (pos >= offset + length) throw new Error('Map: unexpected end');
    const nullFlag = data[pos];
    pos += 1;

    if (nullFlag) {
      result.set(key, null);
    } else {
      const val = readListElement(data, pos, valDesc, decoder);
      pos += elementByteSize(data, pos, valDesc, decoder);
      result.set(key, val);
    }
  }
  return result;
}

// -----------------------------------------------------------------------
// Element helpers for list/map iteration
// -----------------------------------------------------------------------

function readListElement(
  data: Uint8Array,
  pos: number,
  elemDesc: FieldDescriptor,
  decoder: WasmDecoder,
): unknown {
  const wt = elemDesc.wireType;

  if (wt === WireTypeId.STRUCT) {
    // Struct elements: [FIELD_COUNT:2] {fields}*
    // We need to know the byte length. Parse the struct to find it.
    const structData = data.subarray(pos);
    const entries = decoder.decodeStruct(structData);
    const structLen = computeStructByteLength(structData, entries);
    const fakeEntry: FieldEntry = {
      fieldNumber: 0, wireType: wt, isNull: false,
      dataOffset: pos, dataLength: structLen,
    };
    return decodeFieldValue(data, fakeEntry, elemDesc, decoder);
  }

  // Scalar element — encoded raw (no DATA_LEN prefix).
  const size = scalarByteSize(wt);
  if (size > 0) {
    return readScalar(data, pos, size, wt);
  }

  // Variable-length scalar in list context: still has [LEN:4][DATA:LEN].
  return readScalar(data, pos, 0, wt);
}

function elementByteSize(
  data: Uint8Array,
  pos: number,
  elemDesc: FieldDescriptor,
  decoder: WasmDecoder,
): number {
  const wt = elemDesc.wireType;

  if (wt === WireTypeId.STRUCT) {
    const structData = data.subarray(pos);
    const entries = decoder.decodeStruct(structData);
    return computeStructByteLength(structData, entries);
  }

  const fixed = scalarByteSize(wt);
  if (fixed > 0) return fixed;

  // Variable-length: [LEN:4][DATA:LEN]
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const len = view.getUint32(pos, true);
  return 4 + len;
}

function computeStructByteLength(
  structData: Uint8Array,
  entries: FieldEntry[],
): number {
  if (entries.length === 0) return 2; // just the field_count
  // The end of the last field's data (or header if null).
  let maxEnd = 2; // past field_count
  for (const e of entries) {
    if (e.isNull) {
      // The header is at some offset; we track the overall end.
      // Since entries are sequential, the end is past all headers + data.
    } else {
      const end = e.dataOffset + e.dataLength;
      if (end > maxEnd) maxEnd = end;
    }
  }
  // Fallback: scan forward to find actual byte length.
  // The entries' dataOffset is relative to structData.
  // Walk the struct manually.
  const view = new DataView(structData.buffer, structData.byteOffset, structData.byteLength);
  if (structData.byteLength < 2) return 2;
  const fieldCount = view.getUint16(0, true);
  let scanPos = 2;
  for (let i = 0; i < fieldCount; i++) {
    scanPos += 6; // field_num(4) + wire_type(1) + null_flag(1)
    if (scanPos > structData.byteLength) break;
    const nullFlag = structData[scanPos - 1];
    if (!nullFlag) {
      const dataLen = view.getUint32(scanPos, true);
      scanPos += 4 + dataLen;
    }
  }
  return scanPos;
}
