/**
 * Positional row encoder — encodes a plain TypeScript object into the
 * positional wire format using the schema descriptor.
 *
 * Format mirrors positional-decoder.ts:
 *   [NULL_BITFIELD : ceil(num_fields / 8) bytes]
 *   For each non-null field in schema order: payload bytes
 */

import { WireTypeId, scalarByteSize } from './wire-types.js';
import type { FieldDescriptor, SchemaDescriptor } from './schema-descriptor.js';

const textEncoder = new TextEncoder();

// ---------------------------------------------------------------------------
// Write buffer — growable byte array with a cursor.
// ---------------------------------------------------------------------------

class WriteBuf {
  private buf: Uint8Array;
  private view: DataView;
  pos = 0;

  constructor(initialCapacity = 256) {
    this.buf = new Uint8Array(initialCapacity);
    this.view = new DataView(this.buf.buffer);
  }

  private ensure(n: number): void {
    if (this.pos + n <= this.buf.byteLength) return;
    let newSize = this.buf.byteLength * 2;
    while (newSize < this.pos + n) newSize *= 2;
    const newBuf = new Uint8Array(newSize);
    newBuf.set(this.buf);
    this.buf = newBuf;
    this.view = new DataView(this.buf.buffer);
  }

  writeU8(v: number): void {
    this.ensure(1);
    this.buf[this.pos++] = v;
  }

  writeI8(v: number): void {
    this.ensure(1);
    this.view.setInt8(this.pos, v);
    this.pos += 1;
  }

  writeU16(v: number): void {
    this.ensure(2);
    this.view.setUint16(this.pos, v, true);
    this.pos += 2;
  }

  writeI16(v: number): void {
    this.ensure(2);
    this.view.setInt16(this.pos, v, true);
    this.pos += 2;
  }

  writeU32(v: number): void {
    this.ensure(4);
    this.view.setUint32(this.pos, v, true);
    this.pos += 4;
  }

  writeI32(v: number): void {
    this.ensure(4);
    this.view.setInt32(this.pos, v, true);
    this.pos += 4;
  }

  writeU64(v: bigint): void {
    this.ensure(8);
    this.view.setBigUint64(this.pos, v, true);
    this.pos += 8;
  }

  writeI64(v: bigint): void {
    this.ensure(8);
    this.view.setBigInt64(this.pos, v, true);
    this.pos += 8;
  }

  writeF32(v: number): void {
    this.ensure(4);
    this.view.setFloat32(this.pos, v, true);
    this.pos += 4;
  }

  writeF64(v: number): void {
    this.ensure(8);
    this.view.setFloat64(this.pos, v, true);
    this.pos += 8;
  }

  writeBytes(data: Uint8Array): void {
    this.ensure(data.byteLength);
    this.buf.set(data, this.pos);
    this.pos += data.byteLength;
  }

  /** Reserve `n` zero bytes and return the start offset. */
  reserve(n: number): number {
    this.ensure(n);
    const start = this.pos;
    this.buf.fill(0, start, start + n);
    this.pos += n;
    return start;
  }

  /** Set a bit in the buffer at the given byte offset. */
  setBit(byteOffset: number, bitIndex: number): void {
    this.buf[byteOffset + (bitIndex >> 3)] |= 1 << (bitIndex & 7);
  }

  finish(): Uint8Array {
    return this.buf.slice(0, this.pos);
  }
}

// ---------------------------------------------------------------------------
// Bitfield helpers
// ---------------------------------------------------------------------------

function bitfieldBytes(count: number): number {
  return Math.ceil(count / 8);
}

// ---------------------------------------------------------------------------
// Scalar writer
// ---------------------------------------------------------------------------

function writeScalar(w: WriteBuf, wt: WireTypeId, value: unknown): void {
  switch (wt) {
    case WireTypeId.BOOL:
      w.writeU8(value ? 1 : 0);
      break;
    case WireTypeId.INT8:
      w.writeI8(value as number);
      break;
    case WireTypeId.INT16:
      w.writeI16(value as number);
      break;
    case WireTypeId.INT32:
    case WireTypeId.DATE32:
    case WireTypeId.TIME32:
    case WireTypeId.INTERVAL_MONTHS:
      w.writeI32(value as number);
      break;
    case WireTypeId.INT64:
    case WireTypeId.DATE64:
    case WireTypeId.TIME64:
    case WireTypeId.TIMESTAMP_NANO:
    case WireTypeId.DURATION_NANO:
      w.writeI64(value as bigint);
      break;
    case WireTypeId.UINT8:
      w.writeU8(value as number);
      break;
    case WireTypeId.UINT16:
    case WireTypeId.HALF_FLOAT:
      w.writeU16(value as number);
      break;
    case WireTypeId.UINT32:
      w.writeU32(value as number);
      break;
    case WireTypeId.UINT64:
      w.writeU64(value as bigint);
      break;
    case WireTypeId.FLOAT32:
      w.writeF32(value as number);
      break;
    case WireTypeId.FLOAT64:
      w.writeF64(value as number);
      break;
    case WireTypeId.STRING:
    case WireTypeId.LARGE_STRING:
    case WireTypeId.STRING_VIEW: {
      const bytes = textEncoder.encode(value as string);
      w.writeU32(bytes.byteLength);
      w.writeBytes(bytes);
      break;
    }
    case WireTypeId.BINARY:
    case WireTypeId.LARGE_BINARY:
    case WireTypeId.BINARY_VIEW: {
      const data = value as Uint8Array;
      w.writeU32(data.byteLength);
      w.writeBytes(data);
      break;
    }
    case WireTypeId.INTERVAL_DAY_TIME: {
      const v = value as { days: number; milliseconds: number };
      w.writeI32(v.days);
      w.writeI32(v.milliseconds);
      break;
    }
    case WireTypeId.INTERVAL_MONTH_DAY_NANO: {
      const v = value as { months: number; days: number; nanoseconds: bigint };
      w.writeI32(v.months);
      w.writeI32(v.days);
      w.writeI64(v.nanoseconds);
      break;
    }
    case WireTypeId.DECIMAL128:
    case WireTypeId.DECIMAL256:
      w.writeBytes(value as Uint8Array);
      break;
    default:
      throw new Error(`writeScalar: unsupported wire type 0x${wt.toString(16)}`);
  }
}

// ---------------------------------------------------------------------------
// Composite writers
// ---------------------------------------------------------------------------

function writeValue(w: WriteBuf, fd: FieldDescriptor, value: unknown): void {
  const wt = fd.wireType;

  if (wt === WireTypeId.STRUCT) {
    writeStruct(w, fd, value as Record<string, unknown>);
    return;
  }
  if (wt === WireTypeId.LIST || wt === WireTypeId.LARGE_LIST) {
    writeList(w, fd, value as unknown[]);
    return;
  }
  if (wt === WireTypeId.FIXED_SIZE_LIST) {
    writeFixedSizeList(w, fd, value as unknown[]);
    return;
  }
  if (wt === WireTypeId.MAP) {
    writeMap(w, fd, value as Map<unknown, unknown>);
    return;
  }

  writeScalar(w, wt, value);
}

function writeStruct(w: WriteBuf, fd: FieldDescriptor, obj: Record<string, unknown>): void {
  const fields = fd.fields ?? [];
  const n = fields.length;

  const bfStart = w.reserve(bitfieldBytes(n));
  for (let i = 0; i < n; i++) {
    const v = obj[fields[i].name];
    if (v == null) {
      w.setBit(bfStart, i);
    } else {
      writeValue(w, fields[i], v);
    }
  }
}

function writeList(w: WriteBuf, fd: FieldDescriptor, arr: unknown[]): void {
  const elemDesc = fd.element;
  if (!elemDesc) throw new Error('List field missing element descriptor');

  const count = arr.length;
  w.writeU32(count);

  const bfStart = w.reserve(bitfieldBytes(count));
  for (let i = 0; i < count; i++) {
    if (arr[i] == null) {
      w.setBit(bfStart, i);
    } else {
      writeValue(w, elemDesc, arr[i]);
    }
  }
}

function writeFixedSizeList(w: WriteBuf, fd: FieldDescriptor, arr: unknown[]): void {
  const elemDesc = fd.element;
  if (!elemDesc) throw new Error('FixedSizeList field missing element descriptor');
  const count = fd.fixedSize ?? arr.length;

  const bfStart = w.reserve(bitfieldBytes(count));
  for (let i = 0; i < count; i++) {
    if (arr[i] == null) {
      w.setBit(bfStart, i);
    } else {
      writeValue(w, elemDesc, arr[i]);
    }
  }
}

function writeMap(w: WriteBuf, fd: FieldDescriptor, map: Map<unknown, unknown>): void {
  const keyDesc = fd.mapKey;
  const valDesc = fd.mapValue;
  if (!keyDesc || !valDesc) throw new Error('Map field missing key/value descriptors');

  const entries = [...map.entries()];
  const count = entries.length;
  w.writeU32(count);

  // Keys (no null bitfield).
  for (const [key] of entries) {
    writeValue(w, keyDesc, key);
  }

  // Values (with null bitfield).
  const bfStart = w.reserve(bitfieldBytes(count));
  for (let i = 0; i < count; i++) {
    const val = entries[i][1];
    if (val == null) {
      w.setBit(bfStart, i);
    } else {
      writeValue(w, valDesc, val);
    }
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * Encode a plain TypeScript object into the positional wire format.
 */
export function encodePositional(
  schema: SchemaDescriptor,
  values: Record<string, unknown>,
): Uint8Array {
  const w = new WriteBuf();
  const fields = schema.fields;
  const n = fields.length;

  const bfStart = w.reserve(bitfieldBytes(n));
  for (let i = 0; i < n; i++) {
    const v = values[fields[i].name];
    if (v == null) {
      w.setBit(bfStart, i);
    } else {
      writeValue(w, fields[i], v);
    }
  }

  return w.finish();
}
