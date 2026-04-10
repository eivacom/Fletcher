/**
 * Object backend — decodes tagged rows into plain TypeScript objects.
 *
 * Zero runtime dependencies beyond the WASM decoder.
 */

import type { WasmDecoder } from '../wasm-decoder.js';
import type { SchemaDescriptor } from './schema-descriptor.js';
import type { DecoderBackend } from './row-decoder.js';
import { decodeFieldValue } from './row-decoder.js';

export class ObjectBackend implements DecoderBackend<Record<string, unknown>> {
  decode(
    schema: SchemaDescriptor,
    row: Uint8Array,
    decoder: WasmDecoder,
  ): Record<string, unknown> {
    const entries = decoder.decodeRow(row);

    const entryMap = new Map(entries.map(e => [e.fieldNumber, e]));
    const result: Record<string, unknown> = {};
    for (const fd of schema.fields) {
      const entry = entryMap.get(fd.fieldNumber);
      if (!entry) {
        result[fd.name] = null;
      } else {
        result[fd.name] = decodeFieldValue(row, entry, fd, decoder);
      }
    }
    return result;
  }
}
