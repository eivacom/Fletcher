import { describe, it, expect, beforeAll } from 'vitest';
import { WasmDecoder } from '../src/wasm-decoder.js';
import { WireTypeId } from '../src/wire-types.js';

/**
 * Build a tagged row buffer by hand for testing the legacy WasmDecoder.
 */
function buildRow(
  schemaHash: bigint,
  fields: Array<{
    fieldNum: number;
    wireType: number;
    isNull: boolean;
    payload?: Uint8Array;
  }>,
): Uint8Array {
  const parts: number[] = [];

  // Schema hash (8 bytes LE).
  const hBuf = new ArrayBuffer(8);
  const hView = new DataView(hBuf);
  hView.setBigUint64(0, schemaHash, true);
  parts.push(...new Uint8Array(hBuf));

  // Field count (2 bytes LE).
  const cBuf = new ArrayBuffer(2);
  new DataView(cBuf).setUint16(0, fields.length, true);
  parts.push(...new Uint8Array(cBuf));

  for (const f of fields) {
    // field_num (4) + wire_type (1) + null_flag (1)
    const fBuf = new ArrayBuffer(4);
    new DataView(fBuf).setUint32(0, f.fieldNum, true);
    parts.push(...new Uint8Array(fBuf));
    parts.push(f.wireType);
    parts.push(f.isNull ? 1 : 0);

    if (!f.isNull && f.payload) {
      // data_len (4) + payload
      const lBuf = new ArrayBuffer(4);
      new DataView(lBuf).setUint32(0, f.payload.byteLength, true);
      parts.push(...new Uint8Array(lBuf));
      parts.push(...f.payload);
    }
  }

  return new Uint8Array(parts);
}

/** Build a scalar payload for a 32-bit int. */
function int32Payload(v: number): Uint8Array {
  const buf = new ArrayBuffer(4);
  new DataView(buf).setInt32(0, v, true);
  return new Uint8Array(buf);
}

/** Build a scalar payload for a 64-bit float. */
function float64Payload(v: number): Uint8Array {
  const buf = new ArrayBuffer(8);
  new DataView(buf).setFloat64(0, v, true);
  return new Uint8Array(buf);
}

/** Build a string payload: [LEN:4][DATA]. */
function stringPayload(s: string): Uint8Array {
  const encoded = new TextEncoder().encode(s);
  const buf = new Uint8Array(4 + encoded.byteLength);
  new DataView(buf.buffer).setUint32(0, encoded.byteLength, true);
  buf.set(encoded, 4);
  return buf;
}

/** Build a bool payload. */
function boolPayload(v: boolean): Uint8Array {
  return new Uint8Array([v ? 1 : 0]);
}

describe('WasmDecoder (TS fallback)', () => {
  let decoder: WasmDecoder;

  beforeAll(async () => {
    decoder = new WasmDecoder();
    await decoder.init(); // No WASM factory → TS fallback.
  });

  it('uses TS fallback when no WASM factory provided', () => {
    expect(decoder.isWasm).toBe(false);
  });

  it('decodes schema hash', () => {
    const hash = 0x123456789ABCDEF0n;
    const row = buildRow(hash, []);
    expect(decoder.getSchemaHash(row)).toBe(hash);
  });

  it('decodes a row with scalar fields', () => {
    const row = buildRow(1n, [
      { fieldNum: 1, wireType: WireTypeId.INT32, isNull: false, payload: int32Payload(42) },
      { fieldNum: 2, wireType: WireTypeId.FLOAT64, isNull: false, payload: float64Payload(3.14) },
      { fieldNum: 3, wireType: WireTypeId.BOOL, isNull: false, payload: boolPayload(true) },
    ]);

    const entries = decoder.decodeRow(row);
    expect(entries).toHaveLength(3);

    expect(entries[0].fieldNumber).toBe(1);
    expect(entries[0].wireType).toBe(WireTypeId.INT32);
    expect(entries[0].isNull).toBe(false);
    expect(entries[0].dataLength).toBe(4);

    expect(entries[1].fieldNumber).toBe(2);
    expect(entries[1].wireType).toBe(WireTypeId.FLOAT64);
    expect(entries[1].dataLength).toBe(8);

    expect(entries[2].fieldNumber).toBe(3);
    expect(entries[2].wireType).toBe(WireTypeId.BOOL);
    expect(entries[2].dataLength).toBe(1);
  });

  it('decodes null fields', () => {
    const row = buildRow(1n, [
      { fieldNum: 1, wireType: WireTypeId.INT32, isNull: true },
      { fieldNum: 2, wireType: WireTypeId.STRING, isNull: false, payload: stringPayload('hello') },
    ]);

    const entries = decoder.decodeRow(row);
    expect(entries).toHaveLength(2);
    expect(entries[0].isNull).toBe(true);
    expect(entries[0].dataLength).toBe(0);
    expect(entries[1].isNull).toBe(false);
  });

  it('decodes a struct payload', () => {
    // Struct: [FIELD_COUNT:2] {field}*
    const parts: number[] = [];
    // field_count = 1
    const cBuf = new ArrayBuffer(2);
    new DataView(cBuf).setUint16(0, 1, true);
    parts.push(...new Uint8Array(cBuf));
    // field: field_num=5, wire_type=INT32, null=0, data_len=4, payload=99
    const fBuf = new ArrayBuffer(4);
    new DataView(fBuf).setUint32(0, 5, true);
    parts.push(...new Uint8Array(fBuf));
    parts.push(WireTypeId.INT32, 0);
    const lBuf = new ArrayBuffer(4);
    new DataView(lBuf).setUint32(0, 4, true);
    parts.push(...new Uint8Array(lBuf));
    parts.push(...int32Payload(99));

    const structData = new Uint8Array(parts);
    const entries = decoder.decodeStruct(structData);
    expect(entries).toHaveLength(1);
    expect(entries[0].fieldNumber).toBe(5);
    expect(entries[0].wireType).toBe(WireTypeId.INT32);
  });

  it('list/map count helpers work', () => {
    const buf = new Uint8Array(4);
    new DataView(buf.buffer).setUint32(0, 7, true);
    expect(decoder.listCount(buf)).toBe(7);
    expect(decoder.mapCount(buf)).toBe(7);
  });

  it('throws on truncated row', () => {
    expect(() => decoder.decodeRow(new Uint8Array([0]))).toThrow();
  });
});
