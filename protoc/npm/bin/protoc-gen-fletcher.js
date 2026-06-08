#!/usr/bin/env node
// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Node.js shim for the fletcher-protoc plugin. On first invocation it
// downloads the platform-matching native binary from this package's matching
// protoc-v<version> GitHub Release, caches it under
// ~/.cache/protoc-gen-fletcher/<version>/, and exec's it with inherited
// stdin/stdout/stderr so protoc's CodeGeneratorRequest / CodeGeneratorResponse
// pipe protocol passes through the shim unchanged.

'use strict';

const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const https = require('node:https');
const { spawnSync } = require('node:child_process');

const { version: VERSION } = require('../package.json');
const RELEASE_BASE =
  process.env.PROTOC_GEN_FLETCHER_RELEASES_URL ||
  'https://github.com/eivacom/Fletcher/releases/download';

function platformAssetName() {
  const platform = os.platform();
  const arch = os.arch();
  if (platform === 'linux' && arch === 'x64') {
    return 'protoc-gen-fletcher-linux-x64';
  }
  if (platform === 'win32' && arch === 'x64') {
    return 'protoc-gen-fletcher-windows-x64.exe';
  }
  throw new Error(
    `protoc-gen-fletcher: unsupported platform ${platform}/${arch}. ` +
      `Supported: linux/x64, win32/x64.`,
  );
}

function cacheDir() {
  return path.join(os.homedir(), '.cache', 'protoc-gen-fletcher', VERSION);
}

function followRedirects(url, depth) {
  if (depth > 5) {
    return Promise.reject(new Error(`Too many redirects: ${url}`));
  }
  return new Promise((resolve, reject) => {
    https
      .get(url, (res) => {
        if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
          res.resume();
          return resolve(followRedirects(res.headers.location, depth + 1));
        }
        if (res.statusCode !== 200) {
          // 5xx / 429 are transient (gateway timeouts, rate limiting); tag
          // them retryable. 4xx (e.g. 404 missing release) is fatal.
          const err = new Error(
            `Download failed (${res.statusCode}): ${url}\n` +
              `Check that the release exists at ` +
              `https://github.com/eivacom/Fletcher/releases/tag/protoc-v${VERSION}`,
          );
          err.retryable = res.statusCode >= 500 || res.statusCode === 429;
          return reject(err);
        }
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () => resolve(Buffer.concat(chunks)));
        res.on('error', reject);
      })
      .on('error', reject);
  });
}

// GitHub Releases occasionally returns a transient 5xx (e.g. a 504 gateway
// timeout) or drops the connection. Retry a few times with exponential
// backoff so a blip doesn't fail codegen for a user; non-retryable (4xx)
// errors and the final attempt propagate immediately.
const MAX_DOWNLOAD_ATTEMPTS = 6;

function isRetryable(err) {
  // Untagged errors (socket resets, timeouts) are transient; only errors
  // explicitly tagged retryable=false (4xx responses) are fatal.
  return !(err && err.retryable === false);
}

async function downloadWithRetry(url) {
  let lastErr;
  for (let attempt = 1; attempt <= MAX_DOWNLOAD_ATTEMPTS; ++attempt) {
    try {
      return await followRedirects(url, 0);
    } catch (err) {
      lastErr = err;
      if (!isRetryable(err) || attempt === MAX_DOWNLOAD_ATTEMPTS) {
        break;
      }
      const delayMs = Math.min(1000 * 2 ** (attempt - 1), 8000); // 1,2,4,8,8s
      process.stderr.write(
        `protoc-gen-fletcher: download attempt ${attempt}/${MAX_DOWNLOAD_ATTEMPTS} failed ` +
          `(${err.message}); retrying in ${delayMs}ms\n`,
      );
      await new Promise((resolve) => setTimeout(resolve, delayMs));
    }
  }
  throw lastErr;
}

async function ensureBinary() {
  const asset = platformAssetName();
  const cacheBin = path.join(cacheDir(), asset);
  if (fs.existsSync(cacheBin)) {
    return cacheBin;
  }

  const url = `${RELEASE_BASE}/protoc-v${VERSION}/${asset}`;
  process.stderr.write(`protoc-gen-fletcher: downloading ${url}\n`);

  fs.mkdirSync(cacheDir(), { recursive: true });
  const buf = await downloadWithRetry(url);

  // Atomic write: write to a unique tmp file in the same dir, then rename.
  // Multiple npm scripts may invoke the shim in parallel; rename within the
  // same filesystem is atomic so concurrent callers cannot see a half-written
  // file.
  const tmp = `${cacheBin}.tmp.${process.pid}`;
  fs.writeFileSync(tmp, buf, { mode: 0o755 });
  fs.renameSync(tmp, cacheBin);

  return cacheBin;
}

(async () => {
  try {
    const binary = await ensureBinary();
    // stdio: 'inherit' makes the child binary share this Node process's
    // stdin/stdout/stderr handles. protoc spawns us with pipe-based
    // stdin/stdout for the plugin protocol; the binary inherits those exact
    // pipes and reads/writes them directly. Node never touches the bytes.
    const result = spawnSync(binary, process.argv.slice(2), { stdio: 'inherit' });
    process.exit(result.status === null ? 1 : result.status);
  } catch (err) {
    process.stderr.write(`protoc-gen-fletcher: ${err.message}\n`);
    process.exit(1);
  }
})();
