// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { describe, it, expect, beforeAll } from 'vitest';
import { WireTypeId, ObjectBackend, encodePositional } from '../src/index.js';
import type { SchemaDescriptor } from '../src/index.js';

describe('Positional encoder + decoder roundtrip', () => {
  let backend: ObjectBackend;

  beforeAll(() => {
    backend = new ObjectBackend();
  });

  it('round-trips scalar fields', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'a', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false },
        { name: 'b', fieldNumber: 2, wireType: WireTypeId.FLOAT64, nullable: false },
        { name: 'c', fieldNumber: 3, wireType: WireTypeId.BOOL, nullable: false },
        { name: 'd', fieldNumber: 4, wireType: WireTypeId.UINT32, nullable: false },
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
      fields: [{ name: 'name', fieldNumber: 1, wireType: WireTypeId.STRING, nullable: false }],
    };

    const original = { name: 'hello, world!' };
    const encoded = encodePositional(schema, original);
    const decoded = backend.decode(schema, encoded);
    expect(decoded.name).toBe('hello, world!');
  });

  it('round-trips null fields', () => {
    const schema: SchemaDescriptor = {
      fields: [
        { name: 'x', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: true },
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
      fields: [{ name: 'data', fieldNumber: 1, wireType: WireTypeId.BINARY, nullable: false }],
    };

    const blob = new Uint8Array([0xff, 0x00, 0xab, 0xcd]);
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

    const original = {
      tags: new Map([
        ['a', 1],
        ['b', 2],
      ]),
    };
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

// D-BIND-156: the refusal the C++ codecs and the C# gateway client make, with their message and
// C#'s field paths.
describe('Positional encoder: a null where the schema promises a value', () => {
  const leaf = [{ name: 'x', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false }];
  const schema: SchemaDescriptor = {
    fields: [
      { name: 'id', fieldNumber: 1, wireType: WireTypeId.INT32, nullable: false },
      { name: 's', fieldNumber: 2, wireType: WireTypeId.STRUCT, nullable: true, fields: leaf },
      {
        name: 'l',
        fieldNumber: 3,
        wireType: WireTypeId.LIST,
        nullable: false,
        element: {
          name: '',
          fieldNumber: 0,
          wireType: WireTypeId.STRUCT,
          nullable: false,
          fields: leaf,
        },
      },
      {
        name: 'm',
        fieldNumber: 4,
        wireType: WireTypeId.MAP,
        nullable: false,
        mapKey: { name: '', fieldNumber: 0, wireType: WireTypeId.STRING, nullable: false },
        mapValue: {
          name: '',
          fieldNumber: 0,
          wireType: WireTypeId.STRUCT,
          nullable: false,
          fields: leaf,
        },
      },
    ],
  };
  const row = (over: Record<string, unknown>) => ({ id: 1, s: null, l: [], m: new Map(), ...over });
  const refusal = (path: string) =>
    `encodePositional: field '${path}' is not nullable, but the row has a null there; ` +
    'a decoder of this schema has nowhere to put it (D-BIND-156)';

  it('refuses a null in a non-nullable top-level field', () => {
    expect(() => encodePositional(schema, row({ id: null }))).toThrow(refusal('id'));
  });

  it('refuses an absent non-nullable field as a null', () => {
    const { id: _id, ...rest } = row({});
    expect(() => encodePositional(schema, rest)).toThrow(refusal('id'));
  });

  it('refuses a null in a non-nullable field of a present struct, by its path', () => {
    expect(() => encodePositional(schema, row({ s: { x: null } }))).toThrow(refusal('s.x'));
  });

  it('refuses a null in a field of a list element, by its path', () => {
    expect(() => encodePositional(schema, row({ l: [{ x: 1 }, { x: null }] }))).toThrow(
      refusal('l[].x'),
    );
  });

  it('refuses a null in a field of a map value, by its path', () => {
    expect(() => encodePositional(schema, row({ m: new Map([['k', { x: null }]]) }))).toThrow(
      refusal('m{}.value.x'),
    );
  });

  // D-BIND-189: a map key has no null bit on the wire, so a null one is refused, as C++ and C#
  // refuse it (D-BIND-131); before, a string key was written as the text "null".
  it('refuses a null string map key, naming the map', () => {
    expect(() => encodePositional(schema, row({ m: new Map([[null, { x: 1 }]]) }))).toThrow(
      "encodePositional: map key must not be null (field 'm')",
    );
  });

  it('refuses an undefined int map key, naming the map', () => {
    const intKeys: SchemaDescriptor = {
      fields: [
        {
          name: 'counts',
          fieldNumber: 1,
          wireType: WireTypeId.MAP,
          nullable: false,
          mapKey: { name: '', fieldNumber: 0, wireType: WireTypeId.INT32, nullable: false },
          mapValue: { name: '', fieldNumber: 0, wireType: WireTypeId.INT32, nullable: false },
        },
      ],
    };
    expect(() => encodePositional(intKeys, { counts: new Map([[undefined, 1]]) })).toThrow(
      "encodePositional: map key must not be null (field 'counts')",
    );
  });

  it('accepts a null below a null struct: no value at all', () => {
    const decoded = new ObjectBackend().decode(schema, encodePositional(schema, row({ s: null })));
    expect(decoded.s).toBeNull();
  });

  // The native schema marks every list item and map value nullable; the generated element
  // descriptors write `nullable: false` there as a placeholder, which the encoder does not read.
  it('accepts a null list element and a null map value, as the native schema does', () => {
    const decoded = new ObjectBackend().decode(
      schema,
      encodePositional(schema, row({ l: [null], m: new Map([['k', null]]) })),
    ) as { l: unknown[]; m: Map<string, unknown> };
    expect(decoded.l).toEqual([null]);
    expect(decoded.m.get('k')).toBeNull();
  });
});
