/**
 * TypeScript wrapper around the Emscripten-compiled WASM row decoder.
 *
 * Provides a high-level API that hides WASM memory management.
 * When the WASM module is not available (e.g., SSR or test environments),
 * a pure-TypeScript fallback is used.
 */

export const FIELD_ENTRY_SIZE = 14;

/** A single decoded field entry from the field table. */
export interface FieldEntry {
  fieldNumber: number;
  wireType: number;
  isNull: boolean;
  dataOffset: number;
  dataLength: number;
}

/** Read field entries from a field table buffer. */
export function readFieldTable(
  table: DataView,
  fieldCount: number,
): FieldEntry[] {
  const entries: FieldEntry[] = new Array(fieldCount);
  for (let i = 0; i < fieldCount; i++) {
    const off = i * FIELD_ENTRY_SIZE;
    entries[i] = {
      fieldNumber: table.getUint32(off, true),
      wireType:    table.getUint8(off + 4),
      isNull:      table.getUint8(off + 5) !== 0,
      dataOffset:  table.getUint32(off + 6, true),
      dataLength:  table.getUint32(off + 10, true),
    };
  }
  return entries;
}

// -----------------------------------------------------------------------
// Emscripten module interface
// -----------------------------------------------------------------------

interface FletcherDecoderModule {
  _malloc(size: number): number;
  _free(ptr: number): void;
  _fletcher_decode_row(
    dataPtr: number, dataLen: number,
    outPtr: number, outCap: number): number;
  _fletcher_decode_struct(
    dataPtr: number, dataLen: number,
    outPtr: number, outCap: number): number;
  _fletcher_get_schema_hash(
    dataPtr: number, dataLen: number,
    loPtr: number, hiPtr: number): number;
  _fletcher_list_count(dataPtr: number, dataLen: number): number;
  _fletcher_map_count(dataPtr: number, dataLen: number): number;
  HEAPU8: Uint8Array;
}

// -----------------------------------------------------------------------
// Pure-TypeScript fallback decoder
// -----------------------------------------------------------------------

function tsDecodeFields(
  data: Uint8Array,
  pos: number,
  fieldCount: number,
  outTable: Uint8Array,
): number {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const out = new DataView(outTable.buffer, outTable.byteOffset, outTable.byteLength);
  let idx = 0;

  for (let i = 0; i < fieldCount; i++) {
    if (pos + 6 > data.byteLength) return -1;

    const fieldNum  = view.getUint32(pos, true);
    const wireType  = data[pos + 4];
    const nullFlag  = data[pos + 5];
    pos += 6;

    const eOff = idx * FIELD_ENTRY_SIZE;
    if (eOff + FIELD_ENTRY_SIZE > outTable.byteLength) return -1;

    out.setUint32(eOff, fieldNum, true);
    outTable[eOff + 4] = wireType;
    outTable[eOff + 5] = nullFlag;

    if (nullFlag) {
      out.setUint32(eOff + 6, 0, true);
      out.setUint32(eOff + 10, 0, true);
    } else {
      if (pos + 4 > data.byteLength) return -1;
      const dataLen = view.getUint32(pos, true);
      pos += 4;
      if (pos + dataLen > data.byteLength) return -1;
      out.setUint32(eOff + 6, pos, true);
      out.setUint32(eOff + 10, dataLen, true);
      pos += dataLen;
    }
    idx++;
  }
  return idx;
}

// -----------------------------------------------------------------------
// WasmDecoder class
// -----------------------------------------------------------------------

/** Maximum fields we allocate table space for. */
const MAX_FIELDS = 256;

export class WasmDecoder {
  private mod: FletcherDecoderModule | null = null;
  private dataPtr = 0;
  private dataCap = 0;
  private tablePtr = 0;
  private tableCap = MAX_FIELDS * FIELD_ENTRY_SIZE;
  private hashPtr = 0; // 8 bytes for lo+hi uint32s

  /** Load the WASM module.  Call once before decoding. */
  async init(wasmFactory?: () => Promise<FletcherDecoderModule>): Promise<void> {
    if (wasmFactory) {
      this.mod = await wasmFactory();
      this.tablePtr = this.mod._malloc(this.tableCap);
      this.hashPtr  = this.mod._malloc(8);
    }
    // If no factory, we fall back to pure TS.
  }

  /** True when using the WASM backend. */
  get isWasm(): boolean {
    return this.mod !== null;
  }

  /**
   * Decode a full tagged row (with schema_hash header).
   * Returns field entries and the raw data for payload access.
   */
  decodeRow(data: Uint8Array): FieldEntry[] {
    if (this.mod) {
      return this.wasmDecodeRow(data);
    }
    return this.tsDecodeRow(data);
  }

  /**
   * Decode a struct payload (no schema_hash header).
   */
  decodeStruct(data: Uint8Array): FieldEntry[] {
    if (this.mod) {
      return this.wasmDecodeStruct(data);
    }
    return this.tsDecodeStruct(data);
  }

  /** Read the schema hash from a tagged row. */
  getSchemaHash(data: Uint8Array): bigint {
    if (data.byteLength < 8) throw new Error('Row too short for schema hash');
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const lo = view.getUint32(0, true);
    const hi = view.getUint32(4, true);
    return (BigInt(hi) << 32n) | BigInt(lo);
  }

  /** Read list element count. */
  listCount(data: Uint8Array): number {
    if (data.byteLength < 4) return 0;
    return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(0, true);
  }

  /** Read map entry count. */
  mapCount(data: Uint8Array): number {
    if (data.byteLength < 4) return 0;
    return new DataView(data.buffer, data.byteOffset, data.byteLength).getUint32(0, true);
  }

  dispose(): void {
    if (this.mod) {
      if (this.dataPtr) this.mod._free(this.dataPtr);
      if (this.tablePtr) this.mod._free(this.tablePtr);
      if (this.hashPtr) this.mod._free(this.hashPtr);
      this.mod = null;
    }
  }

  // -----------------------------------------------------------------------
  // WASM paths
  // -----------------------------------------------------------------------

  private ensureDataBuf(size: number): void {
    if (size <= this.dataCap) return;
    if (this.dataPtr) this.mod!._free(this.dataPtr);
    this.dataCap = Math.max(size, this.dataCap * 2, 4096);
    this.dataPtr = this.mod!._malloc(this.dataCap);
  }

  private copyToWasm(data: Uint8Array): void {
    this.ensureDataBuf(data.byteLength);
    this.mod!.HEAPU8.set(data, this.dataPtr);
  }

  private readTable(count: number): FieldEntry[] {
    const tableBytes = new Uint8Array(
      this.mod!.HEAPU8.buffer,
      this.tablePtr,
      count * FIELD_ENTRY_SIZE,
    );
    const view = new DataView(
      tableBytes.buffer,
      tableBytes.byteOffset,
      tableBytes.byteLength,
    );
    return readFieldTable(view, count);
  }

  private wasmDecodeRow(data: Uint8Array): FieldEntry[] {
    this.copyToWasm(data);
    const count = this.mod!._fletcher_decode_row(
      this.dataPtr, data.byteLength,
      this.tablePtr, this.tableCap,
    );
    if (count < 0) throw new Error('fletcher_decode_row failed');
    return this.readTable(count);
  }

  private wasmDecodeStruct(data: Uint8Array): FieldEntry[] {
    this.copyToWasm(data);
    const count = this.mod!._fletcher_decode_struct(
      this.dataPtr, data.byteLength,
      this.tablePtr, this.tableCap,
    );
    if (count < 0) throw new Error('fletcher_decode_struct failed');
    return this.readTable(count);
  }

  // -----------------------------------------------------------------------
  // Pure-TypeScript fallback paths
  // -----------------------------------------------------------------------

  private tsDecodeRow(data: Uint8Array): FieldEntry[] {
    if (data.byteLength < 10) throw new Error('Row too short');
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fieldCount = view.getUint16(8, true);
    const table = new Uint8Array(fieldCount * FIELD_ENTRY_SIZE);
    const count = tsDecodeFields(data, 10, fieldCount, table);
    if (count < 0) throw new Error('tsDecodeRow: parse error');
    return readFieldTable(
      new DataView(table.buffer, table.byteOffset, table.byteLength),
      count,
    );
  }

  private tsDecodeStruct(data: Uint8Array): FieldEntry[] {
    if (data.byteLength < 2) throw new Error('Struct too short');
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fieldCount = view.getUint16(0, true);
    const table = new Uint8Array(fieldCount * FIELD_ENTRY_SIZE);
    const count = tsDecodeFields(data, 2, fieldCount, table);
    if (count < 0) throw new Error('tsDecodeStruct: parse error');
    return readFieldTable(
      new DataView(table.buffer, table.byteOffset, table.byteLength),
      count,
    );
  }
}
