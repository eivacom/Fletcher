// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { FletcherClient } from '../src/client.js';

// ---------------------------------------------------------------------------
// Minimal WebSocket stub that lives in the global scope so FletcherClient
// can `new WebSocket(url)`.
// ---------------------------------------------------------------------------
class MockWebSocket {
  static readonly OPEN = 1;
  static readonly CLOSED = 3;

  binaryType = 'arraybuffer';
  readyState = MockWebSocket.OPEN;

  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onmessage: ((event: { data: unknown }) => void) | null = null;

  sent: (string | Uint8Array)[] = [];

  constructor(public url: string) {
    // Schedule onopen so the constructor returns first.
    setTimeout(() => this.onopen?.(), 0);
  }

  send(data: string | Uint8Array): void {
    this.sent.push(data);
  }

  close(): void {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.();
  }
}

// Expose mock on global so `new WebSocket(...)` works inside FletcherClient.
beforeEach(() => {
  (globalThis as any).WebSocket = MockWebSocket;
});
afterEach(() => {
  delete (globalThis as any).WebSocket;
});

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

describe('FletcherClient — connect', () => {
  it('resolves when the WebSocket opens', async () => {
    const client = new FletcherClient({ url: 'ws://localhost:9090' });
    await expect(client.connect()).resolves.toBeUndefined();
    client.close();
  });

  it('rejects when the WebSocket fails to connect', async () => {
    // Override so onopen never fires — onerror fires instead.
    (globalThis as any).WebSocket = class extends MockWebSocket {
      constructor(url: string) {
        super(url);
        // Cancel the queued onopen and fire onerror instead.
        setTimeout(() => this.onerror?.(), 0);
      }
      // Suppress the open scheduled in the parent.
      override set onopen(_fn: (() => void) | null) {
        /* noop */
      }
      override get onopen() {
        return null;
      }
    };

    const client = new FletcherClient({ url: 'ws://localhost:9090' });
    await expect(client.connect()).rejects.toThrow(/connection failed/i);
  });
});

describe('FletcherClient — close', () => {
  it('rejects pending requests when close() is called', async () => {
    const client = new FletcherClient({ url: 'ws://localhost:9090' });
    await client.connect();

    // Start a createTopic that will never receive a server response.
    const pending = client.createTopic('test/topic');

    // Immediately close — the pending promise should reject.
    client.close();
    await expect(pending).rejects.toThrow(/closed/i);
  });

  it('clears subscriptions on close', async () => {
    const client = new FletcherClient({ url: 'ws://localhost:9090' });
    await client.connect();
    // No subscriptions to clear, but close should not throw.
    expect(() => client.close()).not.toThrow();
  });

  it('is safe to call close() multiple times', async () => {
    const client = new FletcherClient({ url: 'ws://localhost:9090' });
    await client.connect();
    client.close();
    expect(() => client.close()).not.toThrow();
  });
});

describe('FletcherClient — sendAndWait guard', () => {
  it('rejects if WebSocket is not connected', async () => {
    const client = new FletcherClient({ url: 'ws://localhost:9090' });
    // Do NOT call connect() — ws is null.
    await expect(client.createTopic('t')).rejects.toThrow(/not connected/i);
  });
});
