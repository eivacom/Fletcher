// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { describe, it, expect, beforeAll } from 'vitest';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ObjectBackend,
  encodePositional,
} from 'fletcher-gateway-client';
import { Telemetry } from '../generated-ts/telemetry.fletcher.js';

interface Vector {
  name: string;
  encoded: string; // base64
}

// Expected input values per scenario name. Kept hardcoded here (rather than
// emitted from C++ alongside the bytes) so the test is self-documenting:
// the file makes it obvious what each scenario asserts without running the
// emitter. Names must match the `Emit("<name>", ...)` calls in
// src/emit_vectors.cpp — adding a new scenario is a two-file change.
const scenarios: Record<string, Record<string, unknown>> = {
  basic: {
    sensor_id: 42,
    temperature: 23.5,
    label: 'intake',
    valid: true,
    readings: [100, 200, 300],
  },
  zero: {
    sensor_id: 0,
    temperature: 0,
    label: '',
    valid: false,
    readings: [],
  },
  negative: {
    sensor_id: -7,
    temperature: -12.75,
    label: 'alpha',
    valid: true,
    readings: [-1, 0, 1],
  },
};

const here = fileURLToPath(new URL('.', import.meta.url));

function findEmitVectorsBinary(): string {
  if (process.env.EMIT_VECTORS_BIN) {
    return process.env.EMIT_VECTORS_BIN;
  }
  // Conan + CMake's `cmake --preset conan-release` puts the binary under
  // build/Release/. Walk a couple of likely layouts.
  const candidates = [
    resolve(here, '..', 'build', 'Release', 'emit_vectors'),
    resolve(here, '..', 'build', 'Release', 'emit_vectors.exe'),
    resolve(here, '..', 'build', 'build', 'Release', 'emit_vectors'),
  ];
  for (const c of candidates) {
    if (existsSync(c)) return c;
  }
  throw new Error(
    `emit_vectors binary not found. Checked: ${candidates.join(', ')}. ` +
    `Set EMIT_VECTORS_BIN to override.`,
  );
}

function inputFor(name: string): Record<string, unknown> {
  const input = scenarios[name];
  if (!input) {
    throw new Error(
      `C++ emitted scenario "${name}" but no expected input is defined ` +
      `in scenarios{}. Add it to byte-compat.test.ts or remove the Emit() ` +
      `call in emit_vectors.cpp.`,
    );
  }
  return input;
}

let vectors: Vector[];

beforeAll(() => {
  const bin = findEmitVectorsBinary();
  const stdout = execFileSync(bin, [], { encoding: 'utf8' });
  vectors = stdout
    .split('\n')
    .map(l => l.trim())
    .filter(l => l.length > 0)
    .map(l => JSON.parse(l) as Vector);
});

describe('protoc + gateway-client-ts byte-compat (telemetry)', () => {
  it('emits exactly the scenarios known to the test', () => {
    const emitted = vectors.map(v => v.name).sort();
    const expected = Object.keys(scenarios).sort();
    expect(emitted).toEqual(expected);
  });

  it('TS decoder reproduces the expected values from C++-encoded bytes', () => {
    const backend = new ObjectBackend();
    for (const vec of vectors) {
      const bytes = Uint8Array.from(Buffer.from(vec.encoded, 'base64'));
      const decoded = backend.decode(Telemetry, bytes);
      expect(decoded, `scenario "${vec.name}"`).toEqual(inputFor(vec.name));
    }
  });

  it('TS encoder produces byte-identical output to C++ for the same input', () => {
    for (const vec of vectors) {
      const tsBytes = encodePositional(Telemetry, inputFor(vec.name));
      const cppBytes = new Uint8Array(Buffer.from(vec.encoded, 'base64'));
      expect(Array.from(tsBytes), `scenario "${vec.name}"`).toEqual(Array.from(cppBytes));
    }
  });
});
