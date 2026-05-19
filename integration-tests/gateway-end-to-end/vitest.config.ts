// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { defineConfig } from 'vitest/config';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = fileURLToPath(new URL('.', import.meta.url));

export default defineConfig({
  test: {
    include: ['test/**/*.test.ts'],
    // The server spawn + heartbeat assertions need a few seconds of
    // headroom; the protoc-gateway-client-ts test uses defaults so
    // raising the timeout only here is fine.
    testTimeout: 30_000,
    hookTimeout: 30_000,
  },
  resolve: {
    alias: {
      // Run against the gateway-client-ts source tree directly so
      // tests pick up local changes without a separate dist/ build.
      'eiva-fletcher-gateway-client': resolve(here, '../../gateway-client-ts/src/index.ts'),
    },
  },
});
