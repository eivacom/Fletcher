// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Pure command-line integration test: drives Google's official protoc
// against the locally-built fletcher-protoc plugin and asserts output.
// No npm packages on the protoc-or-plugin path — only vitest, used as a
// test runner. This mirrors the "raw binary" scenario an end user hits
// when they download protoc-gen-fletcher-<platform> and invoke it
// directly via `--plugin=`, with no Conan environment around it.
import { copyFileSync, chmodSync, existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { spawnSync, type SpawnSyncReturns } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { ensureProtoc } from './util/ensure-protoc.js';
import { findFletcherProtoc } from './util/find-fletcher-protoc.js';

const here: string = dirname(fileURLToPath(import.meta.url));
const projectDir: string = join(here, '..');

let protoc: string = '';
let plugin: string = '';
let outDir: string = '';

beforeAll(async () => {
  protoc = await ensureProtoc();
  plugin = findFletcherProtoc();
  outDir = mkdtempSync(join(tmpdir(), 'fletcher-protoc-cli-'));
});

afterAll(() => {
  if (outDir) {
    rmSync(outDir, { recursive: true, force: true });
  }
});

function runProtoc(pluginPath: string, extraArgs: string[]): SpawnSyncReturns<string> {
  return spawnSync(
    protoc,
    [
      `--plugin=protoc-gen-fletcher=${pluginPath}`,
      `--fletcher_out=${outDir}`,
      '-I',
      join(projectDir, 'proto'),
      ...extraArgs,
      join(projectDir, 'proto', 'simple.proto'),
    ],
    { encoding: 'utf8' },
  );
}

describe('protoc + fletcher-protoc (command line, no npm packages)', () => {
  it('generates the default C++ header for a simple proto', () => {
    const result: SpawnSyncReturns<string> = runProtoc(plugin, []);
    expect(result.status, `stderr: ${result.stderr}`).toBe(0);
    expect(existsSync(join(outDir, 'simple.fletcher.pb.h'))).toBe(true);
  });

  it('generates a TypedSchema .fletcher.ts module when --fletcher_opt=ts is passed', () => {
    const result: SpawnSyncReturns<string> = runProtoc(plugin, ['--fletcher_opt=ts']);
    expect(result.status, `stderr: ${result.stderr}`).toBe(0);

    const tsPath: string = join(outDir, 'simple.fletcher.ts');
    expect(existsSync(tsPath)).toBe(true);

    const tsContent: string = readFileSync(tsPath, 'utf8');
    expect(tsContent).toContain('export const Simple');
    expect(tsContent).toContain('protoPackage');
    expect(tsContent).toContain('protoMessage');
  });

  it('runs standalone when copied to a clean directory with no co-located DLLs', () => {
    // Copy the plugin to a clean temp dir so it has no DLLs nearby. A
    // statically-linked binary must still work; a dynamically-linked one
    // fails with STATUS_DLL_NOT_FOUND / STATUS_ENTRYPOINT_NOT_FOUND on
    // Windows. This is the exact scenario a user hits when they download
    // the raw protoc-gen-fletcher-windows-x64.exe from GitHub Releases
    // and place it under node_modules/.bin/ or a custom directory.
    const isolatedDir: string = mkdtempSync(join(tmpdir(), 'fletcher-protoc-isolated-'));
    const exeBaseName: string =
      process.platform === 'win32' ? 'fletcher-protoc.exe' : 'fletcher-protoc';
    const isolated: string = join(isolatedDir, exeBaseName);
    try {
      copyFileSync(plugin, isolated);
      if (process.platform !== 'win32') {
        chmodSync(isolated, 0o755);
      }
      const result: SpawnSyncReturns<string> = runProtoc(isolated, []);
      expect(
        result.status,
        `Plugin is not standalone — it failed when run from a clean dir with no nearby DLLs.\n` +
          `Build the plugin with: -o "*:shared=False"\n` +
          `stderr: ${result.stderr}`,
      ).toBe(0);
    } finally {
      rmSync(isolatedDir, { recursive: true, force: true });
    }
  });
});
