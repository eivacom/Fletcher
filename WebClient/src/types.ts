/**
 * Shared types used across the client library.
 */

export type BackendType = 'object' | 'arrow';

export interface FletcherClientOptions {
  /** WebSocket URL, e.g. "ws://localhost:9090". */
  url: string;
  /** Which decode/encode backend to use. Default: 'object'. */
  backend?: BackendType;
/** Reconnect delay in ms (0 to disable). Default: 0. */
  reconnectDelay?: number;
}

export type MessageCallback<T = unknown> = (
  message: T,
  attachments: Map<string, Uint8Array>,
) => void;
