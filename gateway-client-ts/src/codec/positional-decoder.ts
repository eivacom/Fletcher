/**
 * Positional row decoder — decodes a positional-format row into a plain
 * TypeScript object using the schema descriptor.
 *
 * Format:
 *   [NULL_BITFIELD : ceil(num_fields / 8) bytes]
 *   For each non-null field in schema order: payload bytes
 */

import { WireTypeId, scalarByteSize } from '../wire-types.js';
import type { FieldDescriptor, SchemaDescriptor } from './schema-descriptor.js';

const textDecoder = new TextDecoder();

// ---------------------------------------------------------------------------
// Bitfield helpers
// ---------------------------------------------------------------------------

function bitfieldBytes(count: number): number {
  return Math.ceil(count / 8);
}

function isNullBit(data: Uint8Array, offset: number, index: number): boolean {
  return (data[offset + (index >> 3)] >> (index & 7) & 1) !== 0;
}

// ---------------------------------------------------------------------------
// Decode context — wraps a Uint8Array with a read cursor.
// ---------------------------------------------------------------------------

class ReadCursor {
  private view: DataView;
  pos: number;

  constructor(
    private data: Uint8Array,
    startPos: number = 0,
  ) {
    this.view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    this.pos = startPos;
  }

  readU8(): number {
    return this.data[this.pos++];
  }

  readI8(): number {
    return this.view.getInt8(this.pos++);
  }

  readU16(): number {
    const v = this.view.getUint16(this.pos, true);
    this.pos += 2;
    return v;
  }

  readI16(): number {
    const v = this.view.getInt16(this.pos, true);
    this.pos += 2;
    return v;
  }

  readU32(): number {
    const v = this.view.getUint32(this.pos, true);
    this.pos += 4;
    return v;
  }

  readI32(): number {
    const v = this.view.getInt32(this.pos, true);
    this.pos += 4;
    return v;
  }

  readU64(): bigint {
    const v = this.view.getBigUint64(this.pos, true);
    this.pos += 8;
    return v;
  }

  readI64(): bigint {
    const v = this.view.getBigInt64(this.pos, true);
    this.pos += 8;
    return v;
  }

  readF32(): number {
    const v = this.view.getFloat32(this.pos, true);
    this.pos += 4;
    return v;
  }

  readF64(): number {
    const v = this.view.getFloat64(this.pos, true);
    this.pos += 8;
    return v;
  }

  readBytes(n: number): Uint8Array {
    const slice = this.data.slice(this.pos, this.pos + n);
    this.pos += n;
    return slice;
  }

  get underlying(): Uint8Array {
    return this.data;
  }
}

// ---------------------------------------------------------------------------
// Scalar reader
// ---------------------------------------------------------------------------

function readScalar(c: ReadCursor, wt: WireTypeId): unknown {
  switch (wt) {
    case WireTypeId.BOOL:
      return c.readU8() !== 0;
    case WireTypeId.INT8:
      return c.readI8();
    case WireTypeId.INT16:
      return c.readI16();
    case WireTypeId.INT32:
    case WireTypeId.DATE32:
    case WireTypeId.TIME32:
    case WireTypeId.INTERVAL_MONTHS:
      return c.readI32();
    case WireTypeId.INT64:
    case WireTypeId.DATE64:
    case WireTypeId.TIME64:
    case WireTypeId.TIMESTAMP_NANO:
    case WireTypeId.DURATION_NANO:
      return c.readI64();
    case WireTypeId.UINT8:
      return c.readU8();
    case WireTypeId.UINT16:
    case WireTypeId.HALF_FLOAT:
      return c.readU16();
    case WireTypeId.UINT32:
      return c.readU32();
    case WireTypeId.UINT64:
      return c.readU64();
    case WireTypeId.FLOAT32:
      return c.readF32();
    case WireTypeId.FLOAT64:
      return c.readF64();
    case WireTypeId.STRING:
    case WireTypeId.LARGE_STRING:
    case WireTypeId.STRING_VIEW: {
      const len = c.readU32();
      const bytes = c.readBytes(len);
      return textDecoder.decode(bytes);
    }
    case WireTypeId.BINARY:
    case WireTypeId.LARGE_BINARY:
    case WireTypeId.BINARY_VIEW: {
      const len = c.readU32();
      return c.readBytes(len);
    }
    case WireTypeId.FIXED_SIZE_BINARY: {
      // Fixed size not known from wire type alone — needs schema info.
      // For now, treat as variable-length with 4-byte length prefix
      // (same as EncodeScalar does NOT prefix fixed-size binary).
      // This case is handled at the call site for fixed-size types.
      throw new Error('FIXED_SIZE_BINARY must be handled at call site');
    }
    case WireTypeId.INTERVAL_DAY_TIME: {
      const days = c.readI32();
      const ms = c.readI32();
      return { days, milliseconds: ms };
    }
    case WireTypeId.INTERVAL_MONTH_DAY_NANO: {
      const months = c.readI32();
      const days = c.readI32();
      const nanos = c.readI64();
      return { months, days, nanoseconds: nanos };
    }
    case WireTypeId.DECIMAL128:
      return c.readBytes(16);
    case WireTypeId.DECIMAL256:
      return c.readBytes(32);
    default:
      throw new Error(`readScalar: unsupported wire type 0x${wt.toString(16)}`);
  }
}

// ---------------------------------------------------------------------------
// Composite readers
// ---------------------------------------------------------------------------

function decodeValue(c: ReadCursor, fd: FieldDescriptor): unknown {
  const wt = fd.wireType;

  if (wt === WireTypeId.STRUCT) {
    return decodeStruct(c, fd);
  }
  if (wt === WireTypeId.LIST || wt === WireTypeId.LARGE_LIST) {
    return decodeList(c, fd);
  }
  if (wt === WireTypeId.FIXED_SIZE_LIST) {
    return decodeFixedSizeList(c, fd);
  }
  if (wt === WireTypeId.MAP) {
    return decodeMap(c, fd);
  }
  if (wt === WireTypeId.SPARSE_UNION || wt === WireTypeId.DENSE_UNION) {
    return decodeUnion(c, fd);
  }

  return readScalar(c, wt);
}

function decodeStruct(c: ReadCursor, fd: FieldDescriptor): Record<string, unknown> {
  const fields = fd.fields ?? [];
  const n = fields.length;
  const bfStart = c.pos;
  c.pos += bitfieldBytes(n);

  const result: Record<string, unknown> = {};
  for (let i = 0; i < n; i++) {
    if (isNullBit(c.underlying, bfStart, i)) {
      result[fields[i].name] = null;
    } else {
      result[fields[i].name] = decodeValue(c, fields[i]);
    }
  }
  return result;
}

function decodeList(c: ReadCursor, fd: FieldDescriptor): unknown[] {
  const elemDesc = fd.element;
  if (!elemDesc) throw new Error('List field missing element descriptor');

  const count = c.readU32();
  const bfStart = c.pos;
  c.pos += bitfieldBytes(count);

  const result: unknown[] = new Array(count);
  for (let i = 0; i < count; i++) {
    if (isNullBit(c.underlying, bfStart, i)) {
      result[i] = null;
    } else {
      result[i] = decodeValue(c, elemDesc);
    }
  }
  return result;
}

function decodeFixedSizeList(c: ReadCursor, fd: FieldDescriptor): unknown[] {
  const elemDesc = fd.element;
  if (!elemDesc) throw new Error('FixedSizeList field missing element descriptor');
  const count = fd.fixedSize ?? 0;

  const bfStart = c.pos;
  c.pos += bitfieldBytes(count);

  const result: unknown[] = new Array(count);
  for (let i = 0; i < count; i++) {
    if (isNullBit(c.underlying, bfStart, i)) {
      result[i] = null;
    } else {
      result[i] = decodeValue(c, elemDesc);
    }
  }
  return result;
}

function decodeMap(c: ReadCursor, fd: FieldDescriptor): Map<unknown, unknown> {
  const keyDesc = fd.mapKey;
  const valDesc = fd.mapValue;
  if (!keyDesc || !valDesc) throw new Error('Map field missing key/value descriptors');

  const count = c.readU32();

  // Keys (no null bitfield).
  const keys: unknown[] = new Array(count);
  for (let i = 0; i < count; i++) {
    keys[i] = decodeValue(c, keyDesc);
  }

  // Values (with null bitfield).
  const valBfStart = c.pos;
  c.pos += bitfieldBytes(count);

  const result = new Map<unknown, unknown>();
  for (let i = 0; i < count; i++) {
    if (isNullBit(c.underlying, valBfStart, i)) {
      result.set(keys[i], null);
    } else {
      result.set(keys[i], decodeValue(c, valDesc));
    }
  }
  return result;
}

function decodeUnion(c: ReadCursor, fd: FieldDescriptor): unknown {
  const typeCode = c.readI8();
  const fields = fd.fields ?? [];
  if (typeCode < 0 || typeCode >= fields.length) {
    throw new Error(`Union: unknown type_code ${typeCode}`);
  }
  return decodeValue(c, fields[typeCode]);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Decode a positional-format row into a plain TypeScript object.
 */
export function decodePositional(
  schema: SchemaDescriptor,
  data: Uint8Array,
): Record<string, unknown> {
  const c = new ReadCursor(data);
  const fields = schema.fields;
  const n = fields.length;

  const bfStart = c.pos;
  c.pos += bitfieldBytes(n);

  const result: Record<string, unknown> = {};
  for (let i = 0; i < n; i++) {
    if (isNullBit(data, bfStart, i)) {
      result[fields[i].name] = null;
    } else {
      result[fields[i].name] = decodeValue(c, fields[i]);
    }
  }
  return result;
}
