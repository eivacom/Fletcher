// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
/**
 * BIND-T-2: one batched subscription's window (D-BIND-183, after D-BIND-133).
 *
 * The rules of the C# gateway client's `GatewayBatcher`, and so of the native
 * `SubscriberArrow`:
 *
 *   - a window flushes at `maxRows` rows (`RowLimit`), when its deadline passes
 *     (`Timeout`), or when the subscription ends (`Closing`);
 *   - the deadline is armed by the window's FIRST event, row or drop, and anchored to
 *     the PREVIOUS flush: a steady stream flushes every `timeoutMs`, and a row after an
 *     idle spell longer than one window is delivered at once;
 *   - a row that does not decode is counted in `rowsDropped` and contributes neither a
 *     row nor its attachments; a window of only dropped rows still delivers, empty.
 *
 * Where it differs from C#, by D-BIND-183: rows arrive decoded (the client has no Arrow
 * backend), so a window is an array of rows, `maxRows` counts good rows, and there is no
 * byte ceiling; JavaScript runs one callback at a time, so no queue is needed for a
 * handler never to run concurrently with itself.
 */

/** Why a window was flushed. */
export type BatchReason = 'RowLimit' | 'Timeout' | 'Closing';

/** Why a window was flushed and whether rows were lost. */
export interface BatchStatus {
  /** Why this window was flushed. */
  reason: BatchReason;
  /** Rows lost since the previous flush; 0 means all good. */
  rowsDropped: number;
}

/** How a batched subscription flushes: the C# `GatewayBatchOptions`' members and defaults. */
export interface BatchOptions {
  /** Flush at this many rows. Default 8000; values below 1 mean 1. */
  maxRows?: number;
  /**
   * ...or when this many milliseconds have passed since the previous flush. Default 60000;
   * `Infinity` for never; otherwise 0 up to {@link MAX_BATCH_TIMEOUT_MS}.
   */
  timeoutMs?: number;
}

/**
 * The longest finite `timeoutMs`: 2^31-1 ms, about 24.8 days, the most `setTimeout` can wait
 * (a longer delay fires at once). C#'s bound is about 49.7 days, its timer's limit.
 */
export const MAX_BATCH_TIMEOUT_MS = 2 ** 31 - 1;

/** Receives one flushed window: row `i`'s attachments at index `i`. */
export type BatchCallback<T> = (
  rows: T[],
  attachments: Map<string, Uint8Array>[],
  status: BatchStatus,
) => void;

/** The options with their defaults, refused when out of range. */
export function resolveBatchOptions(options?: BatchOptions): {
  maxRows: number;
  timeoutMs: number;
} {
  const maxRows = options?.maxRows ?? 8000;
  const timeoutMs = options?.timeoutMs ?? 60_000;
  if (Number.isNaN(maxRows)) {
    throw new RangeError('subscribeBatched: maxRows must be a number');
  }
  if (timeoutMs !== Infinity && !(timeoutMs >= 0 && timeoutMs <= MAX_BATCH_TIMEOUT_MS)) {
    throw new RangeError(
      `subscribeBatched: timeoutMs must be between 0 and ${MAX_BATCH_TIMEOUT_MS} ` +
        '(about 24.8 days, the most a timer can wait): use Infinity for a window with no deadline',
    );
  }
  return { maxRows: Math.max(1, Math.floor(maxRows)), timeoutMs };
}

export class Batcher<T> {
  private rows: T[] = [];
  private attachments: Map<string, Uint8Array>[] = [];
  private dropped = 0;
  private armed = false;
  private lastFlush = performance.now();
  private timer: ReturnType<typeof setTimeout> | undefined;
  private closed = false;

  /**
   * @param deliver Runs the subscription's callback; the client catches what it throws.
   */
  constructor(
    private readonly deliver: (
      rows: T[],
      attachments: Map<string, Uint8Array>[],
      status: BatchStatus,
    ) => void,
    private readonly maxRows: number,
    private readonly timeoutMs: number,
  ) {}

  /** A decoded row for this subscription. */
  add(row: T, attachments: Map<string, Uint8Array>): void {
    if (this.closed) return;
    const due = this.arm();
    this.rows.push(row);
    this.attachments.push(attachments);
    if (this.rows.length >= this.maxRows) {
      this.cut('RowLimit');
    } else if (due) {
      this.cut('Timeout');
    }
  }

  /** A row that did not decode: counted in the window, which it arms. */
  drop(): void {
    if (this.closed) return;
    const due = this.arm();
    ++this.dropped;
    if (due) this.cut('Timeout');
  }

  /** End the subscription: the pending window is flushed as `Closing`, later rows ignored. */
  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.cut('Closing');
  }

  // The window's first event starts its deadline, anchored to the previous flush; true when
  // that deadline has already passed.
  private arm(): boolean {
    if (this.armed) return false;
    this.armed = true;
    if (this.timeoutMs === Infinity) return false;
    const left = this.timeoutMs - (performance.now() - this.lastFlush);
    if (left <= 0) return true;
    // One timer per window, carrying it, so a timer for a window already cut does nothing.
    const window = this.rows;
    this.timer = setTimeout(() => {
      if (!this.closed && window === this.rows) this.cut('Timeout');
    }, left);
    return false;
  }

  // Deliver the window and start the next one.
  private cut(reason: BatchReason): void {
    clearTimeout(this.timer);
    this.timer = undefined;
    this.armed = false;
    this.lastFlush = performance.now();
    if (this.rows.length === 0 && this.dropped === 0) return;
    const rows = this.rows;
    const attachments = this.attachments;
    const rowsDropped = this.dropped;
    this.rows = [];
    this.attachments = [];
    this.dropped = 0;
    this.deliver(rows, attachments, { reason, rowsDropped });
  }
}
