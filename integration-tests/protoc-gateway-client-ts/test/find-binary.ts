// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { readdirSync } from 'node:fs';
import { join } from 'node:path';

// Recursively search `dir` for a file named exactly `name`, returning the
// first match's absolute path (or null if none).
//
// Conan's `cmake_layout` places build artifacts in a different sub-path
// per generator: the Windows multi-config (MSVC) generator emits to
// `build/<config>/...` while the Linux single-config generator emits to
// `build/...`. Rather than enumerate every possible layout, the suite
// locates the spawned binary by name, which is stable across both.
export function findBinaryRecursive(dir: string, name: string): string | null {
  let entries: ReturnType<typeof readdirSync>;
  try {
    entries = readdirSync(dir, { withFileTypes: true });
  } catch {
    return null; // dir doesn't exist yet (e.g. `conan build .` not run)
  }
  for (const entry of entries) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) {
      const found = findBinaryRecursive(full, name);
      if (found) return found;
    } else if (entry.name === name) {
      return full;
    }
  }
  return null;
}
