// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Downloads Google's official pre-built protoc once into a project-local
// cache so the integration test runs hermetically: no system-protoc
// dependency, no npm package providing protoc. Subsequent runs find the
// cached binary and skip the download.
import { existsSync, mkdirSync, writeFileSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { spawnSync } from 'node:child_process';
import { get as httpsGet } from 'node:https';
import type { IncomingMessage } from 'node:http';

// Google switched protobuf to date-based versioning at major bump 21. The
// release tag and asset filenames carry the "21.x" form (e.g. v21.12,
// protoc-21.12-win64.zip) while the C++ library version is still "3.21.x"
// — that's the version we depend on via Conan. PROTOC_RELEASE is the
// release-tag form; the matching C++ runtime is protobuf/3.21.12.
const PROTOC_RELEASE: string = '21.12';

const here: string = dirname(fileURLToPath(import.meta.url));
const projectRoot: string = join(here, '..', '..');

function assetName(): string {
  if (process.platform === 'linux' && process.arch === 'x64') {
    return `protoc-${PROTOC_RELEASE}-linux-x86_64.zip`;
  }
  if (process.platform === 'win32' && process.arch === 'x64') {
    return `protoc-${PROTOC_RELEASE}-win64.zip`;
  }
  throw new Error(
    `Official protoc binary not available for ${process.platform}/${process.arch}. ` +
      `Supported: linux/x64, win32/x64.`,
  );
}

function cacheRoot(): string {
  return join(projectRoot, '.cache', 'protoc', PROTOC_RELEASE);
}

function binaryPath(): string {
  const exe: string = process.platform === 'win32' ? 'protoc.exe' : 'protoc';
  return join(cacheRoot(), 'bin', exe);
}

function followRedirects(url: string, depth: number): Promise<Buffer> {
  if (depth > 5) {
    return Promise.reject(new Error(`Too many redirects: ${url}`));
  }
  return new Promise<Buffer>((resolve, reject) => {
    httpsGet(url, (res: IncomingMessage) => {
      const status: number = res.statusCode ?? 0;
      if (status >= 300 && status < 400 && res.headers.location) {
        res.resume();
        resolve(followRedirects(res.headers.location, depth + 1));
        return;
      }
      if (status !== 200) {
        // 5xx / 429 are transient (GitHub Releases gateway timeouts, rate
        // limiting); tag them retryable. 4xx (e.g. 404 wrong asset) is fatal.
        const err: Error & { retryable?: boolean } = new Error(
          `Download failed (${status}): ${url}`,
        );
        err.retryable = status >= 500 || status === 429;
        reject(err);
        return;
      }
      const chunks: Buffer[] = [];
      res.on('data', (c: Buffer) => chunks.push(c));
      res.on('end', () => resolve(Buffer.concat(chunks)));
      res.on('error', reject);
    }).on('error', reject);
  });
}

// GitHub Releases occasionally returns a transient 5xx (e.g. the 504 seen
// in CI) or drops the connection. Retry a few times with exponential
// backoff so a blip doesn't fail the suite; non-retryable (4xx) errors and
// the final attempt propagate immediately.
const MAX_DOWNLOAD_ATTEMPTS: number = 6;

function isRetryable(err: unknown): boolean {
  // Untagged errors (socket resets, timeouts) are transient; only errors
  // explicitly tagged retryable=false (4xx responses) are fatal.
  return !(err instanceof Error && (err as { retryable?: boolean }).retryable === false);
}

async function downloadWithRetry(url: string): Promise<Buffer> {
  let lastErr: unknown;
  for (let attempt: number = 1; attempt <= MAX_DOWNLOAD_ATTEMPTS; ++attempt) {
    try {
      return await followRedirects(url, 0);
    } catch (err: unknown) {
      lastErr = err;
      if (!isRetryable(err) || attempt === MAX_DOWNLOAD_ATTEMPTS) {
        break;
      }
      const delayMs: number = Math.min(1000 * 2 ** (attempt - 1), 8000); // 1,2,4,8,8s
      const message: string = err instanceof Error ? err.message : String(err);
      process.stderr.write(
        `[ensure-protoc] download attempt ${attempt}/${MAX_DOWNLOAD_ATTEMPTS} failed ` +
          `(${message}); retrying in ${delayMs}ms\n`,
      );
      await new Promise<void>((resolve) => setTimeout(resolve, delayMs));
    }
  }
  throw lastErr;
}

export async function ensureProtoc(): Promise<string> {
  const out: string = binaryPath();
  if (existsSync(out)) {
    return out;
  }

  const asset: string = assetName();
  const url: string = `https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_RELEASE}/${asset}`;
  process.stderr.write(`[ensure-protoc] downloading ${url}\n`);

  const root: string = cacheRoot();
  mkdirSync(root, { recursive: true });

  const buf: Buffer = await downloadWithRetry(url);

  // Stash zip in a tmp file, then extract. Extraction strategy is chosen
  // per-platform for tools that are guaranteed to be present:
  //  - Windows: system tar.exe in System32 handles .zip natively (Win10
  //    1803+) and is invoked by explicit path with argv-array (no shell)
  //    so Git Bash's bsdtar doesn't intercept and misinterpret `C:\path`
  //    as an SSH `host:path`.
  //  - Linux: python3's zipfile module — always present in environments
  //    where this test makes sense (Conan requires python3, so it's
  //    in every Fletcher devcontainer and every CI runner). `unzip` is
  //    not in the Fletcher devcontainer image.
  const zipPath: string = join(tmpdir(), asset);
  writeFileSync(zipPath, buf);

  if (process.platform === 'win32') {
    const systemRoot: string = process.env.SystemRoot ?? 'C:\\Windows';
    const systemTar: string = join(systemRoot, 'System32', 'tar.exe');
    const result = spawnSync(systemTar, ['-xf', zipPath, '-C', root], {
      stdio: 'inherit',
      shell: false,
    });
    if (result.status !== 0) {
      throw new Error(`tar.exe failed extracting ${zipPath} (exit ${result.status})`);
    }
  } else {
    const script: string =
      `import sys, zipfile; ` + `zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])`;
    const result = spawnSync('python3', ['-c', script, zipPath, root], {
      stdio: 'inherit',
      shell: false,
    });
    if (result.status !== 0) {
      throw new Error(`python3 zipfile extraction failed for ${zipPath} (exit ${result.status})`);
    }
  }

  if (!existsSync(out)) {
    throw new Error(`protoc binary not found after extraction at ${out}`);
  }

  // The zip's Linux entries are not always extracted as executable —
  // ensure the bit is set so we can spawn it directly.
  if (process.platform !== 'win32') {
    spawnSync('chmod', ['+x', out], { stdio: 'inherit', shell: false });
  }

  process.stderr.write(`[ensure-protoc] cached at ${out} (${statSync(out).size} bytes)\n`);
  return out;
}
