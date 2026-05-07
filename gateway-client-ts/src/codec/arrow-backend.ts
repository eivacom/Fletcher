/**
 * Arrow JS backend — decodes positional rows into Apache Arrow RecordBatch.
 *
 * Requires `apache-arrow` as a peer dependency.
 * This is a placeholder that will be fully implemented once the
 * object backend is validated.
 */

import type { SchemaDescriptor } from './schema-descriptor.js';
import type { DecoderBackend } from './object-backend.js';

// The arrow import is deferred so the module can be loaded even when
// apache-arrow is not installed (it's an optional peer dep).

export class ArrowBackend implements DecoderBackend<unknown> {
  decode(
    _schema: SchemaDescriptor,
    _row: Uint8Array,
  ): unknown {
    throw new Error(
      'ArrowBackend.decode: not yet implemented. ' +
      'Install apache-arrow and check back after Phase 3.',
    );
  }
}
