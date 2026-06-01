// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Locates the locally-built fletcher-protoc plugin binary in the Conan
// cache. The test fails with a clear remediation message if the plugin
// hasn't been built yet — building is intentionally not done by the
// test itself, so the test stays focused on protoc/plugin behaviour
// rather than build orchestration.
import { existsSync, readdirSync, statSync } from 'node:fs';
import { homedir } from 'node:os';
import { join, sep } from 'node:path';

function exeName(): string {
  return process.platform === 'win32' ? 'fletcher-protoc.exe' : 'fletcher-protoc';
}

function suggestedProfile(): string {
  return process.platform === 'win32'
    ? 'Windows-msvc194-x86_64-Release'
    : 'Linux-gcc13-x86_64-Release';
}

function findRecursive(root: string, target: string, results: string[]): void {
  if (!existsSync(root)) {
    return;
  }
  for (const entry of readdirSync(root, { withFileTypes: true })) {
    const full: string = join(root, entry.name);
    if (entry.isDirectory()) {
      findRecursive(full, target, results);
    } else if (entry.isFile() && entry.name === target) {
      // Only collect binaries in a Conan package's `/bin/` folder. This
      // anchors on the path so we don't pick up intermediate build-folder
      // copies; the Conan package's bin/ is the same layout that consumers
      // (npm shim, conan-package consumers) get.
      const binSegment: string = `${sep}bin${sep}`;
      if (full.includes(binSegment)) {
        results.push(full);
      }
    }
  }
}

export function findFletcherProtoc(): string {
  const root: string = join(homedir(), '.conan2', 'p');
  const target: string = exeName();
  const results: string[] = [];
  findRecursive(root, target, results);

  if (results.length === 0) {
    throw new Error(
      `fletcher-protoc binary not found in Conan cache.\n\n` +
        `Build it first from the repo root (statically linked, no DLL deps):\n` +
        `  conan create protoc/. --build=missing -pr:a=.conan-profiles/${suggestedProfile()} -o "*:shared=False"\n\n` +
        `Searched under: ${root}`,
    );
  }

  // Multiple builds may exist (different option hashes). Pick the most
  // recently modified — that's the one the developer just rebuilt.
  return results
    .map((p: string) => ({ p, mtime: statSync(p).mtimeMs }))
    .sort((a, b) => b.mtime - a.mtime)[0].p;
}
