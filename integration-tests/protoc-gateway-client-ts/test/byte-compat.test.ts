import { describe, it, expect, beforeAll } from 'vitest';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ObjectBackend,
  encodePositional,
} from 'eiva-fletcher-gateway-client';
import { TelemetrySchema } from '../generated-ts/telemetry.fletcher.js';

interface Vector {
  name: string;
  input: Record<string, unknown>;
  encoded: string; // base64
}

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
  it('emits at least one scenario', () => {
    expect(vectors.length).toBeGreaterThan(0);
  });

  it('TS decoder reproduces the original values from C++-encoded bytes', () => {
    const backend = new ObjectBackend();
    for (const vec of vectors) {
      const bytes = Uint8Array.from(Buffer.from(vec.encoded, 'base64'));
      const decoded = backend.decode(TelemetrySchema, bytes);
      expect(decoded, `scenario "${vec.name}"`).toEqual(vec.input);
    }
  });

  it('TS encoder produces byte-identical output to C++ for the same input', () => {
    for (const vec of vectors) {
      const tsBytes = encodePositional(TelemetrySchema, vec.input);
      const cppBytes = new Uint8Array(Buffer.from(vec.encoded, 'base64'));
      expect(Array.from(tsBytes), `scenario "${vec.name}"`).toEqual(Array.from(cppBytes));
    }
  });
});
