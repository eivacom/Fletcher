/**
 * Arrow JS backend — decodes tagged rows into Apache Arrow RecordBatch.
 *
 * Requires `apache-arrow` as a peer dependency.
 * This is a placeholder that will be fully implemented once the
 * object backend is validated.
 */

import type { WasmDecoder } from '../wasm-decoder.js';
import type { SchemaDescriptor } from './schema-descriptor.js';
import type { DecoderBackend } from './row-decoder.js';

// The arrow import is deferred so the module can be loaded even when
// apache-arrow is not installed (it's an optional peer dep).

export class ArrowBackend implements DecoderBackend<unknown> {
  decode(
    _schema: SchemaDescriptor,
    _row: Uint8Array,
    _decoder: WasmDecoder,
  ): unknown {
    throw new Error(
      'ArrowBackend.decode: not yet implemented. ' +
      'Install apache-arrow and check back after Phase 3.',
    );
  }
}
