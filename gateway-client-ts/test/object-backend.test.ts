// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { describe, it, expect, beforeAll } from 'vitest';
import { WireTypeId, ObjectBackend, encodePositional } from '../src/index.js';
import type { SchemaDescriptor } from '../src/index.js';

describe('ObjectBackend (positional)', () => {
  let backend: ObjectBackend;

  beforeAll(() => {
    backend = new ObjectBackend();
  });

  const schema: SchemaDescriptor = {
    fields: [
      { name: 'id', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false },
      { name: 'value', fieldNumber: 2, wireType: WireTypeId.FLOAT64, nullable: false },
      { name: 'label', fieldNumber: 3, wireType: WireTypeId.STRING, nullable: true },
    ],
  };

  it('decodes a complete row to a plain object', () => {
    const row = encodePositional(schema, { id: 10, value: 2.5, label: 'test' });
    const obj = backend.decode(schema, row);
    expect(obj.id).toBe(10);
    expect(obj.value).toBe(2.5);
    expect(obj.label).toBe('test');
  });

  it('returns null for null fields', () => {
    const twoFieldSchema: SchemaDescriptor = {
      fields: [
        { name: 'id', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false },
        { name: 'value', fieldNumber: 2, wireType: WireTypeId.FLOAT64, nullable: true },
        { name: 'label', fieldNumber: 3, wireType: WireTypeId.STRING, nullable: true },
      ],
    };

    const row = encodePositional(twoFieldSchema, { id: 7, value: null, label: null });
    const obj = backend.decode(twoFieldSchema, row);
    expect(obj.id).toBe(7);
    expect(obj.value).toBeNull();
    expect(obj.label).toBeNull();
  });

  it('returns null for explicitly null fields', () => {
    const row = encodePositional(schema, { id: 1, value: 0, label: null });
    const obj = backend.decode(schema, row);
    expect(obj.label).toBeNull();
  });
});
