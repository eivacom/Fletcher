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
    // beforeAll spawns the gateway and the C++ FastDDS peer; the peer's
    // subscribe can poll the DDS schema channel for a few seconds, and DDS
    // discovery between the two participants needs headroom. The per-test
    // deadlines below stay well under testTimeout.
    testTimeout: 30_000,
    hookTimeout: 60_000,
  },
  resolve: {
    alias: {
      // Run against the gateway-client-ts source tree directly so tests
      // pick up local changes without a separate dist/ build.
      'fletcher-gateway-client': resolve(here, '../../gateway-client-ts/src/index.ts'),
    },
  },
});
