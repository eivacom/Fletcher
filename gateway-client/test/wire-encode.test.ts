import { describe, it, expect, beforeAll } from 'vitest';
import { WireTypeId } from '../src/wire-types.js';
import { ObjectBackend } from '../src/codec/object-backend.js';
import { encodePositional } from '../src/codec/positional-encoder.js';
import type { SchemaDescriptor } from '../src/codec/schema-descriptor.js';

describe('Positional encoder + decoder roundtrip', () => {
  let backend: ObjectBackend;

  beforeAll(() => {
    backend = new ObjectBackend();
  });

  it('round-trips scalar fields', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'a', fieldNumber: 1, wireType: WireTypeId.INT32,   nullable: false },
        { name: 'b', fieldNumber: 2, wireType: WireTypeId.FLOAT64, nullable: false },
        { name: 'c', fieldNumber: 3, wireType: WireTypeId.BOOL,    nullable: false },
        { name: 'd', fieldNumber: 4, wireType: WireTypeId.UINT32,  nullable: false },
      ],
    };

    const original = { a: -7, b: 1.23, c: true, d: 999 };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded);

    expect(decoded.a).toBe(-7);
    expect(decoded.b).toBeCloseTo(1.23);
    expect(decoded.c).toBe(true);
    expect(decoded.d).toBe(999);
  });

  it('round-trips 64-bit integers as bigint', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'ts', fieldNumber: 1, wireType: WireTypeId.TIMESTAMP_NANO, nullable: false },
        { name: 'id', fieldNumber: 2, wireType: WireTypeId.UINT64, nullable: false },
      ],
    };

    const original = { ts: 1234567890123456789n, id: 18446744073709551615n };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded);

    expect(decoded.ts).toBe(1234567890123456789n);
    expect(decoded.id).toBe(18446744073709551615n);
  });

  it('round-trips strings', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'name', fieldNumber: 1, wireType: WireTypeId.STRING, nullable: false },
      ],
    };

    const original = { name: 'hello, world!' };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded);
    expect(decoded.name).toBe('hello, world!');
  });

  it('round-trips null fields', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'x', fieldNumber: 1, wireType: WireTypeId.INT32,  nullable: true },
        { name: 'y', fieldNumber: 2, wireType: WireTypeId.STRING, nullable: true },
      ],
    };

    const original = { x: null, y: 'present' };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded);
    expect(decoded.x).toBeNull();
    expect(decoded.y).toBe('present');
  });

  it('round-trips binary data', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'data', fieldNumber: 1, wireType: WireTypeId.BINARY, nullable: false },
      ],
    };

    const blob = new Uint8Array([0xFF, 0x00, 0xAB, 0xCD]);
    const encoded = encodePositional(schema, { data: blob });
    const decoded = backend.decode(schema, encoded);
    expect(decoded.data).toEqual(blob);
  });

  it('round-trips a list of int32', () => {
    const schema: SchemaDescriptor = {
      fields: [
        {
          name: 'nums',
          fieldNumber: 1,
          wireType: WireTypeId.LIST,
          nullable: false,
          element: { name: '', fieldNumber: 0, wireType: WireTypeId.INT32, nullable: false },
        },
      ],
    };

    const original = { nums: [10, 20, 30] };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded);
    expect(decoded.nums).toEqual([10, 20, 30]);
  });

  it('round-trips a map of string to int32', () => {
    const schema: SchemaDescriptor = {
      fields: [
        {
          name: 'tags',
          fieldNumber: 1,
          wireType: WireTypeId.MAP,
          nullable: false,
          mapKey: { name: '', fieldNumber: 0, wireType: WireTypeId.STRING, nullable: false },
          mapValue: { name: '', fieldNumber: 0, wireType: WireTypeId.INT32, nullable: false },
        },
      ],
    };

    const original = { tags: new Map([['a', 1], ['b', 2]]) };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded) as { tags: Map<string, number> };
    expect(decoded.tags.get('a')).toBe(1);
    expect(decoded.tags.get('b')).toBe(2);
  });

  it('produces compact encoding without field headers', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'a', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false },
        { name: 'b', fieldNumber: 2, wireType: WireTypeId.INT32, nullable: false },
      ],
    };

    const encoded = encodePositional(schema, { a: 1, b: 2 });
    // 1 byte null bitfield + 4 bytes + 4 bytes = 9 bytes
    expect(encoded.byteLength).toBe(9);
  });
});
