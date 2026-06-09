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
  },
  resolve: {
    alias: {
      // Resolve protoc-generated TS imports + our test imports to the
      // gateway-client-ts source directly. Avoids needing to build the
      // package's dist/ first.
      '@eiva/fletcher-gateway-client': resolve(here, '../../gateway-client-ts/src/index.ts'),
    },
  },
});
