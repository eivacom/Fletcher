/**
 * Shared types used across the client library.
 */

export type BackendType = 'object' | 'arrow';

export interface FletcherClientOptions {
  /** WebSocket URL, e.g. "ws://localhost:9090". */
  url: string;
  /** Which decode/encode backend to use. Default: 'object'. */
  backend?: BackendType;
  /** Optional WASM module factory (for WASM-accelerated decoding). */
  wasmFactory?: () => Promise<unknown>;
}

export type MessageCallback<T = unknown> = (
  message: T,
  attachments: Map<string, Uint8Array>,
) => void;
