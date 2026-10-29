// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
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
  /**
   * Called when a subscription's callback throws, or a row sent to it does not decode
   * (D-BIND-183, as C#'s `HandlerFaulted`); the subscription goes on either way. What it
   * throws is ignored. Without it, the fault goes to `console.error`.
   */
  onError?: (fault: FletcherClientFault) => void;
}

/** Why `onError` was called: C#'s `GatewayFaultReason`. */
export type FletcherFaultReason = 'HandlerThrew' | 'RowUndecodable';

/** A subscription's fault, reported through `FletcherClientOptions.onError`. */
export interface FletcherClientFault {
  /** What happened. */
  reason: FletcherFaultReason;
  /** The subscription it happened on. */
  subscriptionId: bigint;
  /** What the callback threw, or why the row did not decode. */
  error: unknown;
}

export type MessageCallback<T = unknown> = (
  message: T,
  attachments: Map<string, Uint8Array>,
) => void;
