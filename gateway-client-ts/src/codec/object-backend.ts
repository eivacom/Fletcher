// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * Object backend — decodes positional-format rows into plain TypeScript objects.
 */

import type { SchemaDescriptor } from './schema-descriptor.js';
import { decodePositional } from './positional-decoder.js';

export interface DecoderBackend<T> {
  decode(schema: SchemaDescriptor, row: Uint8Array): T;
}

export class ObjectBackend implements DecoderBackend<Record<string, unknown>> {
  decode(
    schema: SchemaDescriptor,
    row: Uint8Array,
  ): Record<string, unknown> {
    return decodePositional(schema, row);
  }
}
