// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { describe, it, expect } from 'vitest';
import { serializeEnvelope, deserializeEnvelope } from '../src/envelope.js';
import type { Envelope } from '../src/envelope.js';

describe('Envelope', () => {
  it('round-trips an envelope with no attachments', () => {
    const env: Envelope = {
      row: new Uint8Array([1, 2, 3, 4]),
      attachments: new Map(),
    };
    const bytes = serializeEnvelope(env);
    const decoded = deserializeEnvelope(bytes);

    expect(decoded.row).toEqual(env.row);
    expect(decoded.attachments.size).toBe(0);
  });

  it('round-trips an envelope with attachments', () => {
    const env: Envelope = {
      row: new Uint8Array([0xDE, 0xAD]),
      attachments: new Map([
        ['image', new Uint8Array([0xFF, 0xD8, 0xFF])],
        ['meta', new Uint8Array([0x01])],
      ]),
    };
    const bytes = serializeEnvelope(env);
    const decoded = deserializeEnvelope(bytes);

    expect(decoded.row).toEqual(env.row);
    expect(decoded.attachments.size).toBe(2);
    expect(decoded.attachments.get('image')).toEqual(new Uint8Array([0xFF, 0xD8, 0xFF]));
    expect(decoded.attachments.get('meta')).toEqual(new Uint8Array([0x01]));
  });

  it('round-trips an empty envelope', () => {
    const env: Envelope = {
      row: new Uint8Array(0),
      attachments: new Map(),
    };
    const bytes = serializeEnvelope(env);
    const decoded = deserializeEnvelope(bytes);

    expect(decoded.row.byteLength).toBe(0);
    expect(decoded.attachments.size).toBe(0);
  });

  it('throws on truncated buffer', () => {
    expect(() => deserializeEnvelope(new Uint8Array([0, 0]))).toThrow();
  });

  it('matches C++ wire format', () => {
    // Hand-built envelope: row = [0x42], no attachments.
    // Expected wire bytes (little-endian):
    //   row_len=1: [01 00 00 00]
    //   row_data:  [42]
    //   attach_count=0: [00 00 00 00]
    const expected = new Uint8Array([0x01, 0x00, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00]);
    const env: Envelope = {
      row: new Uint8Array([0x42]),
      attachments: new Map(),
    };
    expect(serializeEnvelope(env)).toEqual(expected);

    // Decode it back.
    const decoded = deserializeEnvelope(expected);
    expect(decoded.row).toEqual(new Uint8Array([0x42]));
  });
});
